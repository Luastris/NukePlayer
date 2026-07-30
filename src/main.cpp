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
#include <atomic>
#include <iostream>
namespace bfs = boost::filesystem;
using namespace std;
using namespace nuke;

static const char* kWorld = "scene.nuworld";

// Boot phases (window first, content later): the window opens as soon as the renderer is up;
// assets and the world stream in behind it (worker + async world load) with progress in the
// window title. 0 = pre-boot, 1 = content scan on a worker, 2 = pipelines + world staging /
// activating (driven per frame from onRender), 3 = running.
static std::atomic<int> g_boot{ 0 };

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
    // The whole dist layout resolves against the EXECUTABLE's directory, never the process
    // CWD — a shortcut/Start-menu/terminal launch runs the game with an arbitrary working
    // directory (same rule as Config::baseDir() and the built-in shader dir).
    const bfs::path exeRoot = Config::baseDir();
    std::string pakPath;
    {
        boost::system::error_code ec;
        if      (bfs::exists(exeRoot / "content" / "game.nupak", ec)) pakPath = (exeRoot / "content" / "game.nupak").string();
        else if (bfs::exists(exeRoot / "game.nupak", ec))             pakPath = (exeRoot / "game.nupak").string();
    }
    const bool packed = !pakPath.empty();
    if (packed)
    {
        if (!Package::Mount(pakPath, 0)) { cout << "[player]\t\t" << "Bad package: " << pakPath << ". Aborting." << endl; return 1; }
        // DLC layer between the base (0) and the mods (1000+): content/dlc/*.nupak, each bound
        // to this base by the name recorded in its pak.json (a legacy base has no name — then
        // folder placement is the only binding).
        Package::PakInfo basePak;
        Package::ReadPakInfo(pakPath, basePak);
        Package::MountDlcs(exeRoot.string(), basePak.name);
        // Mods: config/mods.json {"mods": ["mods/foo.numod", ...]} — user-editable next to
        // the game (the project pak stays immutable). MountMods resolves paths tolerantly
        // and orders by DEPENDENCIES (a mod's "requires" from its mod.json mount below it;
        // config order among independents; missing dependency -> the mod is skipped).
        Package::MountMods(exeRoot.string());
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
    static std::string gameTitle = "NukePlayer";   // window title = the PROJECT's name (game.nuproj), not config
                                                   // (static: the frame lambda below reads it for the FPS readout)
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
            bfs::ifstream pf(exeRoot / "project" / "game.nuproj");
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
    if (!packed) DiscoverModulesIn((exeRoot / "project" / "modules").string());
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
    app->contentRoot = packed ? "" : (exeRoot / "project" / "content").string();

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
        // Boot phase 2: finish material pipelines a few per frame (the window keeps pumping —
        // no "Not responding"), then activate the staged world; progress rides the title bar.
        if (g_boot.load() == 2)
        {
            const int left = ResDB::getSingleton()->BuildShaderPipelinesStep(a->render, 3);
            char t[256];
            if (left > 0)
                snprintf(t, sizeof(t), "%s | Compiling shaders... (%d left)", gameTitle.c_str(), left);
            else
            {
                if (a->WorldLoadReady())
                    a->ActivateLoadedWorld();   // World::Update (below) swaps at the frame boundary
                const double lp = a->WorldLoadProgress(), ap = a->WorldActivationProgress();
                if      (ap >= 0) snprintf(t, sizeof(t), "%s | Loading... %d%%", gameTitle.c_str(), 50 + (int)(ap * 50.0));
                else if (lp >= 0) snprintf(t, sizeof(t), "%s | Loading... %d%%", gameTitle.c_str(), (int)(lp * 50.0));
                else
                {
                    // World finished (or failed and logged). Legacy fallback + camera fallback,
                    // exactly what the old synchronous boot did after OpenWorld.
                    if (a->currentWorld->GetHierarchy().empty())
                    {
                        boost::system::error_code ec;
                        if (bfs::exists(bfs::path(kWorld), ec))
                            a->currentWorld->LoadFromFile(kWorld);   // legacy world next to the exe
                    }
                    bool hasCam = false;
                    for (Atom* atom : a->currentWorld->GetHierarchy())
                        if (atom && atom->GetComponent<Camera>()) { hasCam = true; break; }
                    if (!hasCam)
                    {
                        Atom* camAtom = new Atom("Main Camera");
                        Camera* cam = new Camera(camAtom, a->render);   // renderTarget 0 -> backbuffer
                        cam->fov = 60.0f;
                        cam->transform->position = { 0, 0, -5 };
                        a->currentWorld->Add(camAtom);
                        cout << "[player]\t\t" << "World had no camera — added a default Main Camera." << endl;
                    }
                    g_boot = 3;
                    snprintf(t, sizeof(t), "%s", gameTitle.c_str());
                    cout << "[player]\t\t" << "Running." << endl;
                }
            }
            a->render->setWindowTitle(t);
        }
        a->currentWorld->Update();        // per-frame game logic (fixed-step runs on its own thread)
        a->currentWorld->Render(a->render);
        // FPS readout (config window.showFps): rolling average appended to the window title, 2x/sec.
        // Suppressed while booting — the title carries the loading progress then.
        if (g_boot.load() == 3 && a->config && a->config->window.showFps)
        {
            static double acc = 0.0; static int frames = 0;
            acc += Time::getSingleton()->delta;   // REAL seconds (unaffected by game time scale)
            ++frames;
            if (acc >= 0.5)
            {
                char t[320];
                snprintf(t, sizeof(t), "%s | %d FPS (%.1f ms)", gameTitle.c_str(),
                         (int)(frames / acc + 0.5), 1000.0 * acc / frames);
                a->render->setWindowTitle(t);
                acc = 0.0; frames = 0;
            }
        }
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
    wd.rayTracing  = config->window.rayTracing;   // false = force the raster path (window.rayTracing)
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

    // Fixed thread + workers BEFORE the content load: the load itself runs on a worker now.
    // FixedUpdate over the still-empty world is a no-op; physics of streamed-in atoms syncs
    // under the game lock as they activate.
    app->StartFixedThread();   // fixed-frequency update (physics + FixedUpdate), frame-independent
    nuke::Jobs::Init(Config::getSingleton()->jobWorkers, Config::getSingleton()->jobPinCores);   // worker pool (2.4)

    // Load the project's assets in the BACKGROUND: the window is already up and presenting
    // (clear frames + "Loading..." in the title) instead of freezing behind an unpainted
    // window for the whole scan. Worker half = disk/CPU asset registration; the GPU tail
    // (render textures) and the world staging hop back to the game thread — RunOnMain is
    // pumped by World::Update from frame one. Material pipelines compile a few per frame in
    // onRender (g_boot == 2 block above), then the world activates incrementally.
    g_boot = 1;
    {
        const bool packedJob = packed, pakShadersJob = pakShaders;
        const std::string contentRootJob = app->contentRoot, worldJob = startupWorld;
        nuke::Jobs::Schedule([packedJob, pakShadersJob, contentRootJob, worldJob]()
        {
            if (packedJob)
            {
                ResDB::getSingleton()->LoadContentPackaged();
                if (!pakShadersJob) ResDB::getSingleton()->LoadShadersDir("shaders");   // legacy dist: loose shaders/ next to the exe
                ResDB::getSingleton()->LoadShadersPackaged();   // content + built-in shaders straight from pak bytes
            }
            else
            {
                ResDB::getSingleton()->LoadContentDir(contentRootJob);
                ResDB::getSingleton()->LoadShadersDir("shaders");
                ResDB::getSingleton()->LoadShadersDir(contentRootJob);
            }
            if (nuke::Jobs::Stopping()) return;   // window closed mid-load: exit promptly (Shutdown joins us)
            nuke::Jobs::RunOnMain([worldJob]()
            {
                AppInstance* a = AppInstance::GetSingleton();
                ResDB::getSingleton()->CreateRenderTextures(a->render);   // RTs for RenderTextures
                cout << "[player]\t\t" << "Loading default world '" << worldJob << "' (async)..." << endl;
                a->SetWorldActivationBudget(8.0);   // ms/frame: stream atoms in instead of one big hitch
                a->StartWorldLoadAsync(worldJob);   // read+merge+parse on a worker
                g_boot = 2;                         // onRender: pipelines per frame -> activate -> run
            });
        });
    }
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
