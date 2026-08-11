// NukePlayer — the runtime host (ships a game, no editor): loads the renderer module and the
// gameplay plugins, loads a .nuworld, then ticks World::Update / World::Render each frame.
// Cameras render to the backbuffer (target 0), unlike the editor's offscreen RT.

#include <NukeEngine.h>                 // bst/bc aliases + NUKEENGINE_API
#include <interface/AppInstance.h>
#include <input/DesktopInput.h>   // gameplay input provider (keyboard/mouse)
#include <input/Input.h>          // Q6: explicit input-map list from the .nuproj
#include <interface/Modular.h>
#include <interface/Services.h>
#include <config.h>
#include <input/keyboard.h>
#include <input/mouse.h>
#include <API/Model/World.h>
#include <API/Model/Atom.h>
#include <API/Model/resdb.h>
#include <API/Model/Package.h>   // packed game + mod overlays
#include <API/Model/Camera.h>
#include <API/Model/Layers.h>    // render-layer slot names from the project manifest
#include <API/Model/Screen.h>    // live game-screen size (scripts/canvas)
#include <API/Model/Time.h>
#include <API/Model/Jobs.h>     // core job system
#include <API/Model/Log.h>
#include <API/Model/CrashReport.h>   // fatal-failure bundles (config/crash)

#include <nlohmann/json.hpp>
#include <boost/filesystem/fstream.hpp>
#include <vector>
#include <algorithm>
#include <atomic>
#include <cstdlib>   // getenv (dev hooks)
#include <iostream>
namespace bfs = boost::filesystem;
using namespace std;
using namespace nuke;

static const char* kWorld = "scene.nuworld";

// Boot phase: 0 = pre-boot, 1 = content scan on a worker, 2 = pipelines + world staging
// (driven per frame from onRender), 3 = running.
static std::atomic<int> g_boot{ 0 };

int main()
{
    nuke::CrashReport::Install("NukePlayer");   // fatal failures leave a bundle in config/crash
    // DEV HOOK (like NUKE_PACKAGE): NUKE_CRASH_TEST=1 faults immediately — verifies the
    // crash pipeline end-to-end (SEH filter -> minidump -> bundle -> pending marker).
    if (std::getenv("NUKE_CRASH_TEST")) { volatile int* p = nullptr; *p = 1; }
    // CWD = the WRITABLE root before anything reads config: relative writes (config/…,
    // caches) must never land beside or inside an installed bundle. Dev tree and Windows:
    // writableDir == run root — exactly the old behavior. Shipped assets are read through
    // absolute run-root paths (RunRoot/baseDir), not the CWD.
    {
        boost::system::error_code ec;
        bfs::create_directories(nuke::Config::writableDir(), ec);
        bfs::current_path(nuke::Config::writableDir(), ec);
    }
    AppInstance* app = AppInstance::GetSingleton();
    app->setEditor(false);   // plugins see isEditor() == false
    // As early as possible, so a hidden console flashes as little as it can.
    Config::SetConsoleWindowVisible(Config::getSingleton()->window.showConsole);
    nuke::Log::SetConsoleEcho(Config::getSingleton()->logToConsole);
    cout << "[player]\t\t" << "NukePlayer (" << nuke::EngineVersion() << ") starting..." << endl;

    // Packed vs raw: a shipped game carries content/game.nupak, the raw project/ tree is the
    // DEV path only (a release Player refuses to run without a pak). The dist layout resolves
    // against the EXECUTABLE's directory, NEVER the process CWD — launches carry any CWD.
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
        Package::MountPakParts(pakPath, 0);   // the base game's own split parts
        // DLC layer sits between the base (0) and the mods (1000+).
        Package::PakInfo basePak;
        Package::ReadPakInfo(pakPath, basePak);
        Package::MountDlcs(exeRoot.string(), basePak.name);
        Package::MountMods(exeRoot.string());   // config/mods.json, ordered by dependencies
    }
#ifdef NDEBUG
    else
    {
        cout << "[player]\t\t" << "No content/game.nupak found. A release Player runs PACKED games only "
             << "(package the project from the editor: File -> Package Project)." << endl;
        return 1;
    }
#endif

    // The project manifest drives everything below: from the pak when packed, from
    // project/game.nuproj when raw.
    std::string startupWorld = kWorld;
    static std::string gameTitle = "NukePlayer";   // static: the frame lambda below reads it
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
                gameTitle    = pj.value("name", gameTitle);
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
                // Q6: explicit input-map list — set BEFORE the content scan loads .nuinput files.
                if (pj.contains("inputMaps") && pj["inputMaps"].is_array())
                {
                    std::vector<std::string> maps;
                    for (auto& m : pj["inputMaps"]) if (m.is_string()) maps.push_back(m.get<std::string>());
                    nuke::Input::SetEnabledMaps(maps);
                }
            }
        }
    }

    // Startup phase 1 (PHASE_BOOT): discover the pool, enable the project's render provider,
    // get its iRender through the service registry.
    cout << "[player]\t\t" << "Loading modules..." << endl;
    InitModules(app);
    // A RAW project keeps its game modules in <project>/modules; packed games ship theirs
    // next to the exe (already covered above).
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

    // Packed: the Package mount stack owns path resolution. Raw: the dev project tree.
    app->contentRoot = packed ? "" : (exeRoot / "project" / "content").string();

    Config* config = Config::getSingleton();
    app->config   = config;
    app->keyboard = KeyBoard::getSingleton();
    app->mouse    = Mouse::getSingleton();
    app->playState = 1;   // always "playing", so runtime systems like NukeGUI run

    // The per-frame game tick: advance time, run logic, draw.
    render->setOnRender([] {
        AppInstance* a = AppInstance::GetSingleton();
        nuke::Screen::Set(a->render->width, a->render->height);
        Time::getSingleton()->NewFrame();
        // Phase 2: a few material pipelines per frame so the window keeps pumping.
        if (g_boot.load() == 2)
        {
            const int left = ResDB::getSingleton()->BuildShaderPipelinesStep(a->render, 3);
            char t[256];
            if (left > 0)
                snprintf(t, sizeof(t), "%s | Compiling shaders... (%d left)", gameTitle.c_str(), left);
            else
            {
                if (a->WorldLoadReady())
                    a->ActivateLoadedWorld();   // World::Update below swaps at the frame boundary
                const double lp = a->WorldLoadProgress(), ap = a->WorldActivationProgress();
                if      (ap >= 0) snprintf(t, sizeof(t), "%s | Loading... %d%%", gameTitle.c_str(), 50 + (int)(ap * 50.0));
                else if (lp >= 0) snprintf(t, sizeof(t), "%s | Loading... %d%%", gameTitle.c_str(), (int)(lp * 50.0));
                else
                {
                    // The world is in (or failed and logged): legacy + camera fallbacks.
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
        a->currentWorld->Update();        // fixed-step logic runs on its own thread
        a->currentWorld->Render(a->render);
        // FPS readout in the title, 2x/sec. Not while booting: the title carries progress then.
        if (g_boot.load() == 3 && a->config && a->config->window.showFps)
        {
            static double acc = 0.0; static int frames = 0;
            acc += Time::getSingleton()->delta;   // REAL seconds, ignores game time scale
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
    wd.title       = gameTitle.c_str();
    wd.decorated   = config->window.decorated;
    wd.resizable   = config->window.resizable;
    wd.floating    = config->window.floating;
    wd.maximized   = config->window.maximized;
    wd.mode        = config->window.mode;          // 0 windowed / 1 borderless / 2 exclusive fullscreen
    wd.fullscreen  = config->window.fullscreen;
    wd.transparent = config->window.transparent;
    wd.opacity     = config->window.opacity;
    wd.backend     = config->window.backend;   // D3D11 / D3D12
    wd.rayTracing  = config->window.rayTracing;   // false = force the raster path
    wd.gpuValidation = config->gpuValidation;
    wd.clickThrough    = config->window.clickThrough;
    wd.hideFromCapture = config->window.hideFromCapture;

    // Phase 2 (PHASE_RUNTIME): enable the project's plugins. Their OnLoad must register the
    // component types BEFORE the world is deserialized, or those types load as placeholders.
    // No list -> enable everything discovered. Boot providers come from "services" instead.
    // A module a mounted mod/DLC brought is enabled by the MOD being enabled: the game's own
    // plugin list was written before that mod existed and cannot know it. Its DLLs live in the
    // modcache dirs the mount produced — that provenance is the consent.
    auto fromModCache = [](const std::string& modulePath)
    {
        boost::system::error_code ec;
        std::string mp = bfs::absolute(bfs::path(modulePath), ec).generic_string();
        for (char& c : mp) c = (char)tolower((unsigned char)c);
        for (const std::string& d : Package::ModuleCacheDirs())
        {
            std::string dp = bfs::absolute(bfs::path(d), ec).generic_string();
            for (char& c : dp) c = (char)tolower((unsigned char)c);
            if (!dp.empty() && mp.rfind(dp, 0) == 0) return true;
        }
        return false;
    };
    for (auto& m : GetModules())
    {
        if (m->phase() == PHASE_BOOT) continue;
        // Editor tooling never runs in a game. editorTool is ABI 2: never call it on an
        // older DLL, whose vtable lacks the slot.
        if (ModuleAbi(m.get()) >= 2 && m->editorTool()) continue;
        bool want = !haveList || fromModCache(m->modulePath) ||
            std::find(enabledPlugins.begin(), enabledPlugins.end(), m->moduleFile) != enabledPlugins.end();
        if (want) EnablePlugin(m.get());
    }

    // A packed game carries the built-in shaders INSIDE game.nupak; older dists and the raw
    // dev project load them from a loose shaders/ dir.
    const bool pakShaders = packed && !Package::List("shaders/").empty();
    if (pakShaders) LoadBuiltinShadersPackaged(render);
    else            LoadBuiltinShaders(render, "shaders");
    // All four MUST precede init(): they decide sample count, scene format and swapchain output.
    render->setMSAA(msaaSamples);
    render->setHDR(hdrEnabled);
    render->setHDROutput(hdrEnabled);        // HDR10 display output; SDR fallback if unsupported
    render->setHDRNits(hdrPaperWhite, hdrPeak);
    render->init(wd);
    render->setVSync(config->window.vsync);
    nuke::InstallDesktopInput(render);         // keyboard/mouse -> Input controls
    cout << "[player]\t\t" << "Renderer ready." << endl;

    // Both must start BEFORE the content load, which runs on a worker.
    app->StartFixedThread();   // fixed-frequency update: physics + FixedUpdate
    nuke::Jobs::Init(Config::getSingleton()->jobWorkers, Config::getSingleton()->jobPinCores);

    // Background asset load: the worker does disk/CPU registration, RunOnMain does render
    // textures + world staging on the game thread, and pipelines follow in onRender.
    g_boot = 1;
    {
        const bool packedJob = packed, pakShadersJob = pakShaders;
        const std::string contentRootJob = app->contentRoot, worldJob = startupWorld;
        nuke::Jobs::Schedule([packedJob, pakShadersJob, contentRootJob, worldJob]()
        {
            if (packedJob)
            {
                ResDB::getSingleton()->LoadContentPackaged();
                if (!pakShadersJob) ResDB::getSingleton()->LoadShadersDir("shaders");   // legacy dist
                ResDB::getSingleton()->LoadShadersPackaged();
            }
            else
            {
                ResDB::getSingleton()->LoadContentDir(contentRootJob);
                ResDB::getSingleton()->LoadShadersDir("shaders");
                ResDB::getSingleton()->LoadShadersDir(contentRootJob);
            }
            if (nuke::Jobs::Stopping()) return;   // window closed mid-load: let Shutdown's join return
            nuke::Jobs::RunOnMain([worldJob]()
            {
                AppInstance* a = AppInstance::GetSingleton();
                ResDB::getSingleton()->CreateRenderTextures(a->render);
                cout << "[player]\t\t" << "Loading default world '" << worldJob << "' (async)..." << endl;
                a->SetWorldActivationBudget(8.0);   // ms/frame
                a->StartWorldLoadAsync(worldJob);
                g_boot = 2;
            });
        });
    }
    render->loop();

    app->StopFixedThread();
    nuke::Jobs::Shutdown();
    // Do NOT clear the world first: UnloadModules runs the full DisablePlugin per module, so
    // module-owned components must still be reachable while their DLL code is mapped.
    UnloadModules();   // runtime plugins first, then the render provider
    return 0;
}
