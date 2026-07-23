// NukePlayer — the runtime host (ships a game, no editor).
//
// Same host plumbing as the editor minus all editor/UI: it loads the renderer module and
// gameplay plugins, loads a .nuworld, then each frame ticks time, runs game logic
// (World::Update) and renders (World::Render). Cameras render to the backbuffer (target 0),
// so the scene fills the window (the editor instead renders into an offscreen RT).

#include <NukeEngine.h>                 // bst/bc aliases + NUKEENGINE_API
#include <interface/AppInstance.h>
#include <input/DesktopInput.h>   // gameplay input provider (keyboard/mouse)
#include <interface/Modular.h>
#include <interface/Services.h>
#include <config.h>
#include <input/keyboard.h>
#include <input/mouse.h>
#include <API/Model/World.h>
#include <API/Model/Atom.h>
#include <API/Model/resdb.h>
#include <API/Model/Package.h>   // packed game + mod overlays (3.2)
#include <API/Model/Camera.h>
#include <API/Model/Layers.h>    // render-layer slot names from the project manifest
#include <API/Model/Screen.h>    // live game-screen size (scripts/canvas)
#include <API/Model/Time.h>
#include <API/Model/Jobs.h>     // core job system (2.4)
#include <API/Model/Log.h>      // SetConsoleEcho (perf: drop the slow conhost write)

#include <nlohmann/json.hpp>
#include <boost/filesystem/fstream.hpp>
#include <vector>
#include <algorithm>
#include <iostream>
namespace bfs = boost::filesystem;
using namespace std;
using namespace nuke;

static const char* kWorld = "scene.nuworld";

int main()
{
    AppInstance* app = AppInstance::GetSingleton();
    app->setEditor(false);   // not the editor — plugins see isEditor() == false
    // A shipped game hides the OS console window (config window.showConsole=false). Do it
    // early so it flashes as little as possible; a console shared with a terminal is kept.
    Config::SetConsoleWindowVisible(Config::getSingleton()->window.showConsole);
    // Drop the slow OS-console log write when config asks (logToConsole=false) — the Player
    // has no in-app console, so this discards output (no conhost cost).
    nuke::Log::SetConsoleEcho(Config::getSingleton()->logToConsole);
    cout << "[player]\t\t" << "NukePlayer starting..." << endl;

    // Packed vs raw (3.2). A shipped game carries content/game.nupak (the dist layout keeps
    // the root clean); the raw project/ tree is the DEV path and exists only in dev builds —
    // a RELEASE Player refuses to run without a pak (release-release opens no raw projects).
    std::string pakPath;
    {
        boost::system::error_code ec;
        if      (bfs::exists("content/game.nupak", ec)) pakPath = "content/game.nupak";
        else if (bfs::exists("game.nupak", ec))         pakPath = "game.nupak";
    }
    const bool packed = !pakPath.empty();
    if (packed)
    {
        if (!Package::Mount(pakPath, 0)) { cout << "[player]\t\t" << "Bad package: " << pakPath << ". Aborting." << endl; return 1; }
        // Mods: config/mods.json {"mods": ["mods/foo.numod", ...]} — user-editable next to
        // the game (the project pak stays immutable). MountMods resolves paths tolerantly
        // and orders by DEPENDENCIES (a mod's "requires" from its mod.json mount below it;
        // config order among independents; missing dependency -> the mod is skipped).
        Package::MountMods(".");
    }
#ifdef NDEBUG
    else
    {
        cout << "[player]\t\t" << "No content/game.nupak found. A release Player runs PACKED games only "
             << "(package the project from the editor: File -> Package Project)." << endl;
        return 1;
    }
#endif

    // The project manifest drives everything below (default world, AA/HDR, plugin list,
    // service providers) — from the pak when packed, from project/game.nuproj when raw.
    std::string startupWorld = kWorld;
    std::string gameTitle = "NukePlayer";   // window title = the PROJECT's name (game.nuproj), not config
    int   msaaSamples = 4;
    bool  hdrEnabled  = true;
    float hdrPaperWhite = 200.0f, hdrPeak = 1000.0f;
    std::vector<std::string> enabledPlugins; bool haveList = false;
    std::string renderChoice;                // services.render: which dll provides the renderer
    {
        std::string manifest;
        if (packed) Package::Read("game.nuproj", manifest);
        else
        {
            bfs::ifstream pf(bfs::path("project/game.nuproj"));
            if (pf) manifest.assign(std::istreambuf_iterator<char>(pf), std::istreambuf_iterator<char>());
        }
        if (!manifest.empty())
        {
            nlohmann::json pj = nlohmann::json::parse(manifest, nullptr, false);
            if (!pj.is_discarded())
            {
                startupWorld = pj.value("startupWorld", startupWorld);
                gameTitle    = pj.value("name", gameTitle);   // the game's name = the project's name
                if (pj.contains("layers") && pj["layers"].is_array())   // render-layer slot names
                {
                    std::vector<std::string> names;
                    for (auto& n : pj["layers"]) names.push_back(n.is_string() ? n.get<std::string>() : std::string());
                    Layers::SetAll(names);
                }
                msaaSamples  = pj.value("msaa", 4);
                hdrEnabled   = pj.value("hdr", true);
                hdrPaperWhite = pj.value("hdrPaperWhite", 200.0f);
                hdrPeak       = pj.value("hdrPeak", 1000.0f);
                if (pj.contains("plugins") && pj["plugins"].is_array())
                {
                    haveList = true;
                    for (auto& p : pj["plugins"]) enabledPlugins.push_back(p.get<std::string>());
                }
                if (pj.contains("services") && pj["services"].is_object())
                    renderChoice = pj["services"].value("render", std::string());
            }
        }
    }

    // Two-phase startup, phase 1 (PHASE_BOOT): discover the pool, enable the project's
    // render provider, get its iRender through the service registry.
    cout << "[player]\t\t" << "Loading modules..." << endl;
    InitModules(app);
    // A RAW project carries its own game modules in <project>/modules (the 6.0 workflow) —
    // scan them too, or the game's native code never loads outside the editor. Packed games
    // ship their modules next to the exe (already covered by the cwd scan above).
    if (!packed) DiscoverModulesIn("project/modules");
    NUKEModule* renderPlugin = FindServiceProvider("render", renderChoice);
    if (!renderPlugin)
    {
        cout << "[player]\t\t" << "No render provider found in modules/. Aborting." << endl;
        return 1;
    }
    EnablePlugin(renderPlugin);
    iRender* render = GetService<iRender>();
    if (!render)
    {
        cout << "[player]\t\t" << "Render provider '" << renderPlugin->title
             << "' registered no iRender. Aborting." << endl;
        return 1;
    }
    app->render = render;

    // Content paths (scripts etc.): packed -> the Package layer stack owns resolution
    // (ResolveContent consults the mounts); raw -> the dev project tree.
    app->contentRoot = packed ? "" : "project/content";

    Config* config = Config::getSingleton();
    app->config   = config;
    app->keyboard = KeyBoard::getSingleton();
    app->mouse    = Mouse::getSingleton();
    app->playState = 1;   // the Player is always "playing" (so runtime systems like NukeGUI run)

    // The per-frame game tick: advance time, run logic, draw. No editor, no PIE gating —
    // the game logic always runs.
    render->setOnRender([] {
        AppInstance* a = AppInstance::GetSingleton();
        nuke::Screen::Set(a->render->width, a->render->height);   // live game-screen size for scripts/canvas
        Time::getSingleton()->NewFrame();
        a->currentWorld->Update();        // per-frame game logic (fixed-step runs on its own thread)
        a->currentWorld->Render(a->render);
    });

    WindowDesc wd;
    wd.w = config->window.w; wd.h = config->window.h;
    wd.title       = gameTitle.c_str();   // the PROJECT's name (game.nuproj), not a config field
    wd.decorated   = config->window.decorated;
    wd.resizable   = config->window.resizable;
    wd.floating    = config->window.floating;
    wd.maximized   = config->window.maximized;
    wd.mode        = config->window.mode;          // 0 windowed / 1 borderless / 2 exclusive fullscreen
    wd.fullscreen  = config->window.fullscreen;
    wd.transparent = config->window.transparent;
    wd.opacity     = config->window.opacity;
    wd.backend     = config->window.backend;   // D3D11 / D3D12
    wd.gpuValidation = config->gpuValidation;   // Debug GPU validation opt-in (config, double-click friendly)

    // Phase 2 (PHASE_RUNTIME): activate THIS project's chosen plugins (the load list in
    // project/game.nuproj). OnLoad registers their component types BEFORE we deserialize
    // the world; types from plugins not in the list load as inert placeholders. No list ->
    // load everything discovered (a packaged game ships its plugins). Boot providers are
    // driven by "services", not the plugin list — skip them here.
    for (auto& m : GetModules())
    {
        if (m->phase() == PHASE_BOOT) continue;
        // Editor TOOLING modules (asset editors, importers) have no business in a game —
        // skip them even under "no list -> load everything" (a dev pool dir has them all).
        // editorTool is ABI 2: never call it on an older DLL (its vtable lacks the slot).
        if (ModuleAbi(m.get()) >= 2 && m->editorTool()) continue;
        bool want = !haveList ||
            std::find(enabledPlugins.begin(), enabledPlugins.end(), m->moduleFile) != enabledPlugins.end();
        if (want) EnablePlugin(m.get());
    }

    // Built-in shaders: a packed game carries them INSIDE game.nupak ("shaders/" entries) —
    // no loose shaders/ dir ships, and mods can override any of them. Older dists (loose
    // shaders/ next to the exe) and the raw dev project still load from disk.
    const bool pakShaders = packed && !Package::List("shaders/").empty();
    if (pakShaders) LoadBuiltinShadersPackaged(render);
    else            LoadBuiltinShaders(render, "shaders");
    render->setMSAA(msaaSamples);            // before init: pipelines build at the right sample count
    render->setHDR(hdrEnabled);              // before init: scene format (RGBA16F / RGBA8)
    render->setHDROutput(hdrEnabled);        // before init: HDR10 display output (Player only; SDR fallback if no HDR display)
    render->setHDRNits(hdrPaperWhite, hdrPeak);
    render->init(wd);
    render->setVSync(config->window.vsync);   // honour the game's vsync choice (Game.SetVSync persists it)
    nuke::InstallDesktopInput(render);         // gameplay input: keyboard/mouse -> Input controls
    cout << "[player]\t\t" << "Renderer ready." << endl;

    // Load the project's assets so the world resolves meshGuid/matGuid/shaderGuid references.
    if (packed)
    {
        ResDB::getSingleton()->LoadContentPackaged();
        if (!pakShaders) ResDB::getSingleton()->LoadShadersDir("shaders");   // legacy dist: loose shaders/ next to the exe
        ResDB::getSingleton()->LoadShadersPackaged();   // content + built-in shaders straight from pak bytes
    }
    else
    {
        ResDB::getSingleton()->LoadContentDir(app->contentRoot);
        ResDB::getSingleton()->LoadShadersDir("shaders");
        ResDB::getSingleton()->LoadShadersDir(app->contentRoot);
    }
    ResDB::getSingleton()->BuildShaderPipelines(render);
    ResDB::getSingleton()->CreateRenderTextures(render);   // RTs for RenderTextures

    // Load the project's default world from content (game: load project -> load its default world).
    cout << "[player]\t\t" << "Loading default world '" << startupWorld << "'..." << endl;
    if (!app->OpenWorld(startupWorld))
        app->currentWorld->LoadFromFile(kWorld);   // fallback: legacy world next to the exe

    // Fallback: a world authored in the editor has no camera (the editor camera is excluded
    // from saves). Add a default one so the game still shows something.
    bool hasCam = false;
    for (Atom* atom : app->currentWorld->GetHierarchy())
        if (atom && atom->GetComponent<Camera>()) { hasCam = true; break; }
    if (!hasCam)
    {
        Atom* camAtom = new Atom("Main Camera");
        Camera* cam = new Camera(camAtom, render);   // renderTarget defaults to 0 -> backbuffer
        cam->fov = 60.0f;
        cam->transform->position = { 0, 0, -5 };
        app->currentWorld->Add(camAtom);
        cout << "[player]\t\t" << "World had no camera — added a default Main Camera." << endl;
    }

    cout << "[player]\t\t" << "Running." << endl;
    app->StartFixedThread();   // fixed-frequency update (physics + FixedUpdate), frame-independent
    nuke::Jobs::Init(Config::getSingleton()->jobWorkers, Config::getSingleton()->jobPinCores);   // worker pool (2.4)
    render->loop();

    app->StopFixedThread();
    nuke::Jobs::Shutdown();
    // UnloadModules runs the FULL DisablePlugin per module: live module-owned components
    // (game modules!) are downgraded/destroyed while their DLL code is still mapped —
    // clearing the world here instead would HIDE them from that downgrade and leak
    // module-code std::functions (Events subscriptions) into the CRT teardown.
    UnloadModules();   // runtime plugins first, then the render provider (its Shutdown deinits)
    return 0;
}
