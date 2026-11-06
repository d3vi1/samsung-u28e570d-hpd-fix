#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <algorithm>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "monitorfw/core.hpp"
#include "monitorfw/storage.hpp"
#include "monitorfw/transport.hpp"
#include "mock_flash.hpp"

#ifndef MONITORFW_ENABLE_HARDWARE
#define MONITORFW_ENABLE_HARDWARE 0
#endif

namespace {
using namespace monitorfw;
const UINT UiEventMessage = WM_APP + 1;
const UINT WorkerFinishedMessage = WM_APP + 2;
const UINT SmokeNextMessage = WM_APP + 3;
enum ControlId { Mode = 100, Devices, Refresh, Kind, FirmwarePath, BrowseFirmware,
    ManifestPath, BrowseManifest, OutputPath, BrowseOutput, Download, Upload, Verify, Cancel, Eeprom };
enum class Operation { Refresh, Download, Upload, Verify };
enum class EventKind { Log, Progress, Plan, Devices };

struct UiEvent {
    EventKind kind;
    std::string text;
    monitorfw::Progress progress;
    std::vector<DisplayEndpoint> devices;
    explicit UiEvent(EventKind value) : kind(value) {}
};
struct AppState {
    HWND window = 0, title = 0, modeLabel = 0, mode = 0, deviceLabel = 0, devicesControl = 0;
    HWND refresh = 0, identity = 0, kindLabel = 0, kind = 0, firmwareLabel = 0, firmware = 0;
    HWND firmwareBrowse = 0, manifestLabel = 0, manifest = 0, manifestBrowse = 0;
    HWND outputLabel = 0, output = 0, outputBrowse = 0, download = 0, upload = 0, verify = 0;
    HWND cancel = 0, eeprom = 0, eepromReason = 0, progress = 0, status = 0, log = 0;
    HFONT font = 0, titleFont = 0;
    HANDLE worker = 0, decisionEvent = 0;
    volatile LONG cancellation = 0, decision = 0, mutationLocked = 0;
    volatile LONG workerFinished = 0;
    std::string completionText;
    bool operationSucceeded = false, smoke = false;
    int smokeStep = 0, exitCode = 0;
    std::wstring smokeRoot, smokeFirstRun;
    int dpi = 96;
    std::string lastPhase;
    std::vector<DisplayEndpoint> endpoints;
    std::vector<ProviderInfo> providers;
    Bytes mockImage;
};
struct Work {
    AppState* app;
    Operation operation;
    bool mock;
    std::string providerId;
    ImageKind kind;
    DisplayEndpoint endpoint;
    std::wstring inputPath, evidencePath, outputFolder;
};

int scale(const AppState& app, int value) { return MulDiv(value,app.dpi,96); }
bool busy(const AppState& app) { return app.worker != 0; }
bool mockMode(const AppState& app) { return SendMessageW(app.mode,CB_GETCURSEL,0,0) <= 0; }
ProviderInfo selectedProvider(const AppState& app) {
    const LRESULT selected = SendMessageW(app.mode,CB_GETCURSEL,0,0);
    if (selected <= 0 || static_cast<std::size_t>(selected-1) >= app.providers.size()) return ProviderInfo();
    return app.providers[static_cast<std::size_t>(selected-1)];
}
bool ownDump(const AppState& app) { return SendMessageW(app.kind,CB_GETCURSEL,0,0) == 1; }
std::wstring textOf(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::vector<wchar_t> buffer(static_cast<std::size_t>(length) + 1);
    GetWindowTextW(control,buffer.data(),static_cast<int>(buffer.size()));
    return std::wstring(buffer.data());
}
void appendLog(AppState& app, const std::string& message) {
    const std::wstring text = widenUtf8(message) + L"\r\n";
    SendMessageW(app.log,EM_SETSEL,static_cast<WPARAM>(-1),static_cast<LPARAM>(-1));
    SendMessageW(app.log,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>(text.c_str()));
    SendMessageW(app.log,EM_SCROLLCARET,0,0);
}
void post(AppState& app, std::unique_ptr<UiEvent> event) {
    if (PostMessageW(app.window,UiEventMessage,0,reinterpret_cast<LPARAM>(event.get()))) event.release();
    else if (event->kind == EventKind::Plan) throw Error("The UI could not receive the programming plan; execution was withheld.");
}
void postText(AppState& app, EventKind kind, const std::string& text) {
    std::unique_ptr<UiEvent> event(new UiEvent(kind)); event->text = text; post(app,std::move(event));
}
void updateControls(AppState& app) {
    const bool idle = !busy(app);
    for (HWND control : {app.mode,app.devicesControl,app.refresh,app.kind,app.firmware,
        app.firmwareBrowse,app.output,app.outputBrowse,app.download,app.upload,app.verify}) {
        EnableWindow(control,idle);
    }
    EnableWindow(app.manifest,idle && ownDump(app));
    EnableWindow(app.manifestBrowse,idle && ownDump(app));
    EnableWindow(app.cancel,!idle && InterlockedCompareExchange(&app.mutationLocked,0,0) == 0);
    EnableWindow(app.eeprom,FALSE);
    const bool available = mockMode(app) || (MONITORFW_ENABLE_HARDWARE && selectedProvider(app).implemented);
    EnableWindow(app.refresh,idle && available);
    for (HWND control : {app.download,app.upload,app.verify}) EnableWindow(control,idle && available && !app.endpoints.empty());
}
void showIdentity(AppState& app) {
    const LRESULT selected = SendMessageW(app.devicesControl,CB_GETCURSEL,0,0);
    if (selected < 0 || static_cast<std::size_t>(selected) >= app.endpoints.size()) {
        SetWindowTextW(app.identity,L"Select Refresh to list endpoints. No device is opened automatically."); return;
    }
    const MonitorIdentity& id = app.endpoints[static_cast<std::size_t>(selected)].identity;
    std::string text = (mockMode(app) ? "SYNTHETIC DEVICE | Firmware profile: " : "Firmware profile: ") + id.model +
        " | Serial: " + id.serial + "\r\nEDID SHA-256: " + id.edidSha256;
    if (id.serialAmbiguous) text += "\r\nAmbiguous identity: " + id.ambiguityReason;
    SetWindowTextW(app.identity,widenUtf8(text).c_str());
}
void fillEndpoints(AppState& app, const std::vector<DisplayEndpoint>& endpoints) {
    app.endpoints = endpoints;
    SendMessageW(app.devicesControl,CB_RESETCONTENT,0,0);
    for (const DisplayEndpoint& endpoint : endpoints) {
        const std::string label = (mockMode(app) ? "[MOCK] " : "") + endpoint.identity.model + " | " +
            endpoint.identity.serial + " | " + endpoint.identity.transportLocator;
        SendMessageW(app.devicesControl,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(widenUtf8(label).c_str()));
    }
    if (!endpoints.empty()) SendMessageW(app.devicesControl,CB_SETCURSEL,0,0);
    showIdentity(app);
}
std::vector<DisplayEndpoint> mockEndpoints() {
    DisplayEndpoint endpoint; endpoint.providerId = "mock"; endpoint.identity = testing::syntheticIdentity();
    return std::vector<DisplayEndpoint>(1,endpoint);
}

std::wstring freshRunPath(const std::wstring& folder, bool mock) {
    if (folder.empty()) throw Error("Choose an output folder first.");
    const DWORD attributes = GetFileAttributesW(folder.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        throw Error("The output folder does not exist.");
    FILETIME time; GetSystemTimeAsFileTime(&time);
    std::wostringstream leaf;
    leaf << (mock ? L"mock-run-" : L"run-") << std::hex << time.dwHighDateTime << L"-" << time.dwLowDateTime
         << L"-" << GetCurrentProcessId();
    return folder + ((folder.back() == L'\\' || folder.back() == L'/') ? L"" : L"\\") + leaf.str();
}
std::string utf8File(const Bytes& bytes) { return std::string(bytes.begin(),bytes.end()); }

DWORD WINAPI workerMain(LPVOID parameter) {
    std::unique_ptr<Work> work(static_cast<Work*>(parameter));
    AppState& app = *work->app;
    std::unique_ptr<IFlashDevice> device;
    testing::MockFlash* mockDevice = 0;
    std::string completion;
    app.operationSucceeded = false;
    try {
        if (work->operation == Operation::Refresh) {
            std::unique_ptr<UiEvent> event(new UiEvent(EventKind::Devices));
            if (work->mock) event->devices = mockEndpoints();
            else {
#if MONITORFW_ENABLE_HARDWARE
                std::unique_ptr<II2cProvider> provider = createProvider(work->providerId);
                if (!provider->info().implemented) throw Error(provider->info().reason);
                event->devices = provider->enumerate();
#else
                throw Error("Hardware access is disabled in this development build.");
#endif
            }
            completion = work->mock ? "Mock endpoint refreshed; no hardware access." : "Explicit endpoint refresh completed.";
            app.operationSucceeded = true;
            post(app,std::move(event));
        } else {
            Bytes input;
            BackupEvidence evidence;
            const BackupEvidence* evidencePointer = 0;
            if (work->operation != Operation::Download) {
                if (work->inputPath.empty() && work->mock && work->kind == ImageKind::SamsungPackage) {
                    input = work->operation == Operation::Verify
                        ? packageFromDump(app.mockImage.empty() ? testing::syntheticDump() : app.mockImage)
                        : testing::syntheticPackage("1000.1",128);
                    postText(app,EventKind::Log,"Using an in-memory SYNTHETIC package; no Samsung firmware was substituted.");
                } else {
                    if (work->inputPath.empty()) throw Error("Choose a firmware or full SPI file.");
                    input = readLocalFile(work->inputPath);
                }
                if (work->kind == ImageKind::SamsungPackage) {
                    const PackageInfo package = inspectPackage(input);
                    postText(app,EventKind::Log,"Container checked: " + package.version + " | SHA-256 " + package.sha256);
                } else {
                    if (input.size() != SpiCapacity) throw Error("A full SPI backup must contain exactly 2 MiB.");
                    if (work->evidencePath.empty()) throw Error("Choose the matching backup manifest for an own SPI dump.");
                    evidence = parseBackupManifest(utf8File(readLocalFile(work->evidencePath,1024 * 1024)));
                    evidencePointer = &evidence;
                }
            }
            if (InterlockedCompareExchange(&app.cancellation,0,0)) throw Error("Cancelled before opening a device.");
            const std::wstring runPath = freshRunPath(work->outputFolder,work->mock);
            if (app.smoke && work->operation == Operation::Download) app.smokeFirstRun = runPath;
            std::unique_ptr<IArtifactStore> store = makeWindowsArtifactStore(runPath,work->mock);
            postText(app,EventKind::Log,(work->mock ? "MOCK artifacts: " : "Artifacts: ") + narrowUtf8(runPath));
            if (work->mock) {
                mockDevice = new testing::MockFlash(app.mockImage.empty() ? testing::syntheticDump() : app.mockImage,
                    work->endpoint.identity);
                device.reset(mockDevice);
            } else {
#if MONITORFW_ENABLE_HARDWARE
                std::unique_ptr<II2cProvider> provider = createProvider(work->providerId);
                if (!provider->info().implemented) throw Error(provider->info().reason);
                device = makeMstarFlash(provider->open(work->endpoint),work->endpoint.identity);
#else
                throw Error("Hardware access is disabled in this development build.");
#endif
            }
            ProgressSink progress = [&app](const monitorfw::Progress& value) {
                std::unique_ptr<UiEvent> event(new UiEvent(EventKind::Progress)); event->progress = value;
                post(app,std::move(event));
            };
            CancelCheck cancellation = [&app]() { return InterlockedCompareExchange(&app.cancellation,0,0) != 0; };
            Session session(*device,*store,work->endpoint.identity,progress,cancellation);
            Result result;
            if (work->operation == Operation::Download) {
                session.backupTwice();
                result = session.finishBackup();
            } else if (work->operation == Operation::Verify) {
                session.backupTwice();
                result = session.verify(input,work->kind,evidencePointer);
            } else {
                const Backup backup = session.backupTwice();
                const FirmwarePolicy policy = work->mock
                    ? testing::syntheticPolicy(std::vector<Bytes>{packageFromDump(backup.image),
                        work->kind == ImageKind::SamsungPackage ? input : packageFromDump(input)})
                    : reviewedPolicy();
                {
                    const FlashPlan plan = work->kind == ImageKind::SamsungPackage
                        ? planPackageOverlay(backup,input,policy) : planOwnDump(backup,input,evidence,policy);
                    const std::string planJson = planManifest(plan);
                    store->saveNew("plan-review.json",Bytes(planJson.begin(),planJson.end()));
                    std::ostringstream summary;
                    summary << (work->mock ? "MOCK ONLY: synthetic flash memory" : "Selected physical monitor")
                        << "\nFirmware profile: " << backup.monitor.model << "\nSerial: " << backup.monitor.serial
                        << "\nCompared backup reads: 2\nChanged sectors: " << plan.sectors().size()
                        << "\nSource SHA-256: " << backup.firstSha256 << "\nTarget SHA-256: " << plan.targetHash()
                        << "\nPlan SHA-256: " << plan.planHash()
                        << "\n\nApply this exact plan? Cancellation is locked during final preflight and programming.";
                    postText(app,EventKind::Log,planManifest(plan));
                    postText(app,EventKind::Plan,summary.str());
                    WaitForSingleObject(app.decisionEvent,INFINITE);
                    if (InterlockedCompareExchange(&app.decision,0,0) != 1 || cancellation()) {
                        postText(app,EventKind::Log,"Plan declined. Closing the verified backup session without programming.");
                        result = session.finishBackup();
                    } else {
                        InterlockedExchange(&app.mutationLocked,1);
                        postText(app,EventKind::Log,"Confirmed plan. Cancellation locked; final preflight and programming are owned by the engine.");
                        result = session.execute(plan);
                    }
                }
            }
            postText(app,EventKind::Log,resultManifest(result));
            app.operationSucceeded = result.success;
            completion = (work->mock ? "MOCK: " : "") + std::string(result.success ? "Operation verified." : "Operation did not complete successfully.");
            if (result.recoveryRequired) completion += " Recovery is required; no implicit reset will be attempted.";
            if (!result.error.empty()) completion += " " + result.error;
        }
    } catch (const std::exception& error) {
        completion = (work->mock ? "MOCK: " : "") + std::string("Stopped: ") + error.what() +
            " Inspect the run artifacts. No destructor resets or retries the device.";
    } catch (...) {
        completion = "Stopped after an unexpected error. Inspect artifacts; no implicit reset is performed.";
    }
    if (mockDevice) {
        try { app.mockImage = mockDevice->image; } catch (...) { completion += " Mock state could not be retained."; }
    }
    // No heap allocation or queued event is required for completion. The UI timer
    // checks the owned thread handle as a fallback if PostMessage cannot enqueue.
    app.completionText.swap(completion);
    InterlockedExchange(&app.workerFinished,1);
    PostMessageW(app.window,WorkerFinishedMessage,0,0);
    return 0;
}

void beginOperation(AppState& app, Operation operation) {
    if (busy(app)) return;
    std::unique_ptr<Work> work(new Work);
    work->app = &app; work->operation = operation; work->mock = mockMode(app);
    if (app.smoke && !work->mock) throw Error("Smoke tests cannot select hardware.");
    if (!work->mock) {
        const ProviderInfo provider = selectedProvider(app);
        if (!provider.implemented || !MONITORFW_ENABLE_HARDWARE) {
            const std::string reason = !provider.implemented ? provider.reason : "Hardware access is disabled in this development build.";
            MessageBoxW(app.window,widenUtf8(reason).c_str(),L"Provider unavailable",MB_OK | MB_ICONINFORMATION); return;
        }
        work->providerId = provider.id;
    }
    work->kind = ownDump(app) ? ImageKind::OwnSpiDump : ImageKind::SamsungPackage;
    work->inputPath = textOf(app.firmware); work->evidencePath = textOf(app.manifest); work->outputFolder = textOf(app.output);
    if (operation != Operation::Refresh) {
        const LRESULT selected = SendMessageW(app.devicesControl,CB_GETCURSEL,0,0);
        if (selected < 0 || static_cast<std::size_t>(selected) >= app.endpoints.size()) {
            MessageBoxW(app.window,L"Refresh and select an endpoint first.",L"Select a monitor",MB_OK | MB_ICONINFORMATION); return;
        }
        if (work->outputFolder.empty()) {
            MessageBoxW(app.window,L"Choose an output folder for the independent reads and reports.",L"Output folder",MB_OK | MB_ICONINFORMATION); return;
        }
        work->endpoint = app.endpoints[static_cast<std::size_t>(selected)];
    }
    InterlockedExchange(&app.cancellation,0); InterlockedExchange(&app.decision,0);
    InterlockedExchange(&app.mutationLocked,0); ResetEvent(app.decisionEvent); app.lastPhase.clear();
    InterlockedExchange(&app.workerFinished,0); app.completionText.clear();
    SendMessageW(app.progress,PBM_SETPOS,0,0); SetWindowTextW(app.status,L"Starting worker...");
    app.worker = CreateThread(0,0,workerMain,work.get(),0,0);
    if (!app.worker) { MessageBoxW(app.window,L"The worker thread could not be created.",L"Start failed",MB_OK | MB_ICONERROR); return; }
    work.release(); updateControls(app);
}
void requestCancel(AppState& app) {
    if (!busy(app) || InterlockedCompareExchange(&app.mutationLocked,0,0)) return;
    InterlockedExchange(&app.cancellation,1); InterlockedExchange(&app.decision,-1); SetEvent(app.decisionEvent);
    SetWindowTextW(app.status,L"Cancellation requested. Waiting for the engine's safe stopping point...");
    EnableWindow(app.cancel,FALSE);
}

void finishSmoke(AppState& app, bool success) {
    app.exitCode = success ? 0 : 1;
    std::ostringstream report;
    report << "{\"test\":\"ui_mock_smoke\",\"passed\":" << app.smokeStep
        << ",\"success\":" << (success ? "true" : "false")
        << ",\"hardwareAccessed\":false,\"uacRequested\":false}\n";
    const std::string text = report.str();
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    if (output && output != INVALID_HANDLE_VALUE) WriteFile(output,text.data(),static_cast<DWORD>(text.size()),&written,0);
    const std::wstring path = app.smokeRoot + L"\\smoke-result.json";
    const HANDLE file = CreateFileW(path.c_str(),GENERIC_WRITE,0,0,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,0);
    if (file != INVALID_HANDLE_VALUE) {
        if (!WriteFile(file,text.data(),static_cast<DWORD>(text.size()),&written,0) || written != text.size() || !FlushFileBuffers(file)) app.exitCode = 1;
        CloseHandle(file);
    } else app.exitCode = 1;
    DestroyWindow(app.window);
}
void finishWorkerIfReady(AppState& app) {
    if (!busy(app) || !InterlockedCompareExchange(&app.workerFinished,0,0) ||
        WaitForSingleObject(app.worker,0) != WAIT_OBJECT_0) return;
    CloseHandle(app.worker); app.worker = 0;
    appendLog(app,app.completionText); SetWindowTextW(app.status,widenUtf8(app.completionText).c_str()); updateControls(app);
    if (app.smoke) {
        if (!app.operationSucceeded) { finishSmoke(app,false); return; }
        ++app.smokeStep;
        if (app.smokeStep == 6) finishSmoke(app,true);
        else if (!PostMessageW(app.window,SmokeNextMessage,0,0)) finishSmoke(app,false);
    }
}
void nextSmokeOperation(AppState& app) {
    if (!app.smoke || busy(app)) return;
    if (app.smokeStep == 0) beginOperation(app,Operation::Download);
    else if (app.smokeStep == 2) beginOperation(app,Operation::Upload);
    else if (app.smokeStep == 4) {
        SendMessageW(app.kind,CB_SETCURSEL,1,0);
        SetWindowTextW(app.firmware,(app.smokeFirstRun + L"\\backup-read-1.bin").c_str());
        SetWindowTextW(app.manifest,(app.smokeFirstRun + L"\\backup.json").c_str());
        beginOperation(app,Operation::Upload);
    } else beginOperation(app,Operation::Verify);
}

void browseFile(AppState& app, HWND destination, bool manifest) {
    std::vector<wchar_t> path(32768,0);
    OPENFILENAMEW options = {}; options.lStructSize = sizeof(options); options.hwndOwner = app.window;
    options.lpstrFile = path.data(); options.nMaxFile = static_cast<DWORD>(path.size());
    options.lpstrFilter = manifest ? L"Backup manifest (*.json)\0*.json\0All files\0*.*\0\0"
        : L"Firmware / SPI image (*.bin)\0*.bin\0All files\0*.*\0\0";
    options.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&options)) SetWindowTextW(destination,path.data());
}
void browseFolder(AppState& app) {
    BROWSEINFOW options = {}; options.hwndOwner = app.window;
    options.lpszTitle = L"Choose a folder for a new operation directory";
    options.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST selection = SHBrowseForFolderW(&options);
    if (selection) {
        wchar_t path[MAX_PATH] = {};
        if (SHGetPathFromIDListW(selection,path)) SetWindowTextW(app.output,path);
        CoTaskMemFree(selection);
    }
}
HWND control(AppState& app, const wchar_t* type, const wchar_t* text, DWORD style, int id = 0) {
    HWND window = CreateWindowExW(0,type,text,WS_CHILD | WS_VISIBLE | style,0,0,1,1,
        app.window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(0),0);
    if (window) SendMessageW(window,WM_SETFONT,reinterpret_cast<WPARAM>(app.font),TRUE);
    return window;
}
void createControls(AppState& app) {
    HDC dc = GetDC(app.window); if (dc) { app.dpi = GetDeviceCaps(dc,LOGPIXELSX); ReleaseDC(app.window,dc); }
    NONCLIENTMETRICSW metrics = {}; metrics.cbSize = sizeof(metrics);
    LOGFONTW font = {};
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0)) font = metrics.lfMessageFont;
    else { font.lfHeight = -scale(app,13); lstrcpynW(font.lfFaceName,L"Tahoma",LF_FACESIZE); }
    app.font = CreateFontIndirectW(&font); font.lfWeight = FW_BOLD; font.lfHeight = -scale(app,21);
    app.titleFont = CreateFontIndirectW(&font);
    app.title = control(app,L"STATIC",L"Samsung monitor firmware",SS_LEFT);
    SendMessageW(app.title,WM_SETFONT,reinterpret_cast<WPARAM>(app.titleFont),TRUE);
    app.modeLabel = control(app,L"STATIC",L"Provider",SS_LEFT);
    app.mode = control(app,L"COMBOBOX",L"",CBS_DROPDOWNLIST | WS_TABSTOP,Mode);
    SendMessageW(app.mode,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"MOCK / OFFLINE - synthetic memory"));
    app.providers = listProviders(); // Registry metadata only; no driver initialization.
    for (const ProviderInfo& provider : app.providers) {
        const std::string label = provider.name + (!provider.implemented ? " - unavailable" :
            (MONITORFW_ENABLE_HARDWARE ? " - explicit Refresh required" : " - disabled in this build"));
        SendMessageW(app.mode,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(widenUtf8(label).c_str()));
    }
    SendMessageW(app.mode,CB_SETCURSEL,0,0);
    app.refresh = control(app,L"BUTTON",L"Refresh",BS_PUSHBUTTON | WS_TABSTOP,Refresh);
    app.deviceLabel = control(app,L"STATIC",L"Monitor",SS_LEFT);
    app.devicesControl = control(app,L"COMBOBOX",L"",CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,Devices);
    app.identity = control(app,L"EDIT",L"",ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL);
    app.kindLabel = control(app,L"STATIC",L"Image type",SS_LEFT);
    app.kind = control(app,L"COMBOBOX",L"",CBS_DROPDOWNLIST | WS_TABSTOP,Kind);
    SendMessageW(app.kind,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Samsung firmware package"));
    SendMessageW(app.kind,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Own full SPI dump + matching manifest"));
    SendMessageW(app.kind,CB_SETCURSEL,0,0);
    app.firmwareLabel = control(app,L"STATIC",L"Firmware / SPI file (empty uses a generated synthetic package in mock)",SS_LEFT);
    app.firmware = control(app,L"EDIT",L"",WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,FirmwarePath);
    app.firmwareBrowse = control(app,L"BUTTON",L"Browse...",BS_PUSHBUTTON | WS_TABSTOP,BrowseFirmware);
    app.manifestLabel = control(app,L"STATIC",L"Matching backup manifest (required for own SPI dumps)",SS_LEFT);
    app.manifest = control(app,L"EDIT",L"",WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,ManifestPath);
    app.manifestBrowse = control(app,L"BUTTON",L"Browse...",BS_PUSHBUTTON | WS_TABSTOP,BrowseManifest);
    app.outputLabel = control(app,L"STATIC",L"Output folder - each operation creates a fresh directory",SS_LEFT);
    app.output = control(app,L"EDIT",L"",WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,OutputPath);
    app.outputBrowse = control(app,L"BUTTON",L"Browse...",BS_PUSHBUTTON | WS_TABSTOP,BrowseOutput);
    app.download = control(app,L"BUTTON",L"Read / download",BS_PUSHBUTTON | WS_TABSTOP,Download);
    app.upload = control(app,L"BUTTON",L"Upload / restore",BS_PUSHBUTTON | WS_TABSTOP,Upload);
    app.verify = control(app,L"BUTTON",L"Verify",BS_PUSHBUTTON | WS_TABSTOP,Verify);
    app.cancel = control(app,L"BUTTON",L"Cancel",BS_PUSHBUTTON | WS_TABSTOP,Cancel);
    app.eeprom = control(app,L"BUTTON",L"Coding EEPROM",BS_PUSHBUTTON,Eeprom);
    app.eepromReason = control(app,L"STATIC",widenUtf8(codingEepromUnavailableReason()).c_str(),SS_LEFT);
    app.progress = control(app,PROGRESS_CLASSW,L"",0); SendMessageW(app.progress,PBM_SETRANGE32,0,1000);
    app.status = control(app,L"STATIC",MONITORFW_ENABLE_HARDWARE
        ? L"Ready in MOCK mode. Hardware requires explicit provider selection and Refresh."
        : L"Ready in MOCK mode. Hardware access is disabled in this development build.",SS_LEFT);
    app.log = control(app,L"EDIT",L"",WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL);
    SendMessageW(app.log,EM_SETLIMITTEXT,4 * 1024 * 1024,0);
    fillEndpoints(app,mockEndpoints()); updateControls(app);
    appendLog(app,"MOCK / OFFLINE: generated firmware and identity only. No monitor or NVIDIA API is accessed at startup.");
}
void layout(AppState& app, int width, int height) {
    const int margin = scale(app,18), right = width - margin, browseWidth = scale(app,92), gap = scale(app,8);
    auto move = [&app](HWND control, int x, int y, int w, int h) { MoveWindow(control,x,scale(app,y),w,scale(app,h),TRUE); };
    move(app.title,margin,16,right-margin,30);
    move(app.modeLabel,margin,61,scale(app,90),22);
    move(app.mode,margin+scale(app,94),56,right-margin-scale(app,94)-browseWidth-gap,150);
    move(app.refresh,right-browseWidth,56,browseWidth,26);
    move(app.deviceLabel,margin,97,scale(app,90),22);
    move(app.devicesControl,margin+scale(app,94),92,right-margin-scale(app,94),180);
    move(app.identity,margin,128,right-margin,52);
    move(app.kindLabel,margin,192,scale(app,90),22);
    move(app.kind,margin+scale(app,94),187,right-margin-scale(app,94),130);
    move(app.firmwareLabel,margin,224,right-margin,20);
    move(app.firmware,margin,246,right-margin-browseWidth-gap,26);
    move(app.firmwareBrowse,right-browseWidth,246,browseWidth,26);
    move(app.manifestLabel,margin,280,right-margin,20);
    move(app.manifest,margin,302,right-margin-browseWidth-gap,26);
    move(app.manifestBrowse,right-browseWidth,302,browseWidth,26);
    move(app.outputLabel,margin,336,right-margin,20);
    move(app.output,margin,358,right-margin-browseWidth-gap,26);
    move(app.outputBrowse,right-browseWidth,358,browseWidth,26);
    int x = margin;
    for (HWND button : {app.download,app.upload,app.verify}) { move(button,x,400,scale(app,144),30); x += scale(app,152); }
    move(app.cancel,right-browseWidth,400,browseWidth,30);
    move(app.eeprom,margin,444,scale(app,144),28);
    move(app.eepromReason,margin+scale(app,156),442,right-margin-scale(app,156),44);
    move(app.progress,margin,494,right-margin,20);
    move(app.status,margin,522,right-margin,26);
    MoveWindow(app.log,margin,scale(app,554),right-margin,std::max(scale(app,40),height-scale(app,554)-margin),TRUE);
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    AppState* app = reinterpret_cast<AppState*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<AppState*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        app->window = window; SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window,message,wParam,lParam);
    switch (message) {
    case WM_CREATE:
        createControls(*app);
        if (!SetTimer(window,1,100,0)) return -1;
        return 0;
    case WM_SIZE: layout(*app,LOWORD(lParam),HIWORD(lParam)); return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        limits->ptMinTrackSize.x = scale(*app,760); limits->ptMinTrackSize.y = scale(*app,680); return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case Refresh: beginOperation(*app,Operation::Refresh); break;
        case Download: beginOperation(*app,Operation::Download); break;
        case Upload: beginOperation(*app,Operation::Upload); break;
        case Verify: beginOperation(*app,Operation::Verify); break;
        case Cancel: requestCancel(*app); break;
        case BrowseFirmware: browseFile(*app,app->firmware,false); break;
        case BrowseManifest: browseFile(*app,app->manifest,true); break;
        case BrowseOutput: browseFolder(*app); break;
        case Kind: if (HIWORD(wParam) == CBN_SELCHANGE) updateControls(*app); break;
        case Devices: if (HIWORD(wParam) == CBN_SELCHANGE) showIdentity(*app); break;
        case Mode:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                fillEndpoints(*app,mockMode(*app) ? mockEndpoints() : std::vector<DisplayEndpoint>());
                const ProviderInfo provider = selectedProvider(*app);
                const std::string status = mockMode(*app) ? "MOCK mode selected." :
                    (!provider.implemented ? provider.reason : (MONITORFW_ENABLE_HARDWARE
                        ? "Provider selected without I/O. Press Refresh explicitly to enumerate."
                        : "Provider metadata only. Hardware access is disabled in this build."));
                SetWindowTextW(app->status,widenUtf8(status).c_str()); updateControls(*app);
            }
            break;
        } return 0;
    case UiEventMessage: {
        std::unique_ptr<UiEvent> event(reinterpret_cast<UiEvent*>(lParam));
        if (event->kind == EventKind::Progress) {
            const monitorfw::Progress& value = event->progress;
            const WPARAM position = value.total ? static_cast<WPARAM>(1000ULL * value.done / value.total) : 0;
            SendMessageW(app->progress,PBM_SETPOS,position,0);
            std::ostringstream status; status << value.phase << " " << value.done << "/" << value.total;
            SetWindowTextW(app->status,widenUtf8(status.str()).c_str());
            if (app->lastPhase != value.phase) { app->lastPhase = value.phase; appendLog(*app,value.phase); }
            updateControls(*app);
        } else if (event->kind == EventKind::Devices) {
            fillEndpoints(*app,event->devices);
        } else if (event->kind == EventKind::Plan) {
            int answer = IDNO;
            if (app->smoke && mockMode(*app)) answer = IDYES;
            else if (!InterlockedCompareExchange(&app->cancellation,0,0)) {
                answer = MessageBoxW(window,widenUtf8(event->text).c_str(),L"Review the exact programming plan",MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING);
            }
            InterlockedExchange(&app->decision,answer == IDYES ? 1 : -1); SetEvent(app->decisionEvent);
        } else { appendLog(*app,event->text); updateControls(*app); }
        return 0;
    }
    case WorkerFinishedMessage: finishWorkerIfReady(*app); return 0;
    case WM_TIMER: if (wParam == 1) finishWorkerIfReady(*app); return 0;
    case SmokeNextMessage: nextSmokeOperation(*app); return 0;
    case WM_QUERYENDSESSION: return busy(*app) ? FALSE : TRUE;
    case WM_CLOSE:
        if (busy(*app)) {
            if (InterlockedCompareExchange(&app->mutationLocked,0,0)) {
                MessageBoxW(window,L"Programming or final verification is active. Keep the application open until the engine finishes.",L"Operation in progress",MB_OK | MB_ICONINFORMATION);
            } else if (MessageBoxW(window,L"Request cancellation and wait for the operation to finish? The window stays open until cleanup is complete.",L"Operation in progress",MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) == IDYES) requestCancel(*app);
            return 0;
        }
        DestroyWindow(window); return 0;
    case WM_DESTROY: KillTimer(window,1); PostQuitMessage(app->exitCode); return 0;
    }
    return DefWindowProcW(window,message,wParam,lParam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    INITCOMMONCONTROLSEX controls = {sizeof(controls),ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
    if (!InitCommonControlsEx(&controls)) return 1;
    const HRESULT ole = OleInitialize(0);
    AppState app;
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(),&argumentCount);
    if (!arguments) { if (SUCCEEDED(ole)) OleUninitialize(); return 1; }
    app.smoke = argumentCount == 2 && lstrcmpW(arguments[1],L"--smoke-test") == 0;
    const bool invalidArguments = argumentCount > 1 && !app.smoke;
    LocalFree(arguments);
    if (invalidArguments) { if (SUCCEEDED(ole)) OleUninitialize(); return 2; }
    if (app.smoke) {
        wchar_t temporary[MAX_PATH] = {};
        const DWORD length = GetTempPathW(MAX_PATH,temporary);
        if (!length || length >= MAX_PATH) { if (SUCCEEDED(ole)) OleUninitialize(); return 1; }
        app.smokeRoot = freshRunPath(temporary,true);
        if (!CreateDirectoryW(app.smokeRoot.c_str(),0)) { if (SUCCEEDED(ole)) OleUninitialize(); return 1; }
    }
    app.decisionEvent = CreateEventW(0,TRUE,FALSE,0);
    if (!app.decisionEvent) { if (SUCCEEDED(ole)) OleUninitialize(); return 1; }
    WNDCLASSEXW type = {}; type.cbSize = sizeof(type); type.lpfnWndProc = windowProcedure;
    type.hInstance = instance; type.hCursor = LoadCursorW(0,IDC_ARROW); type.hIcon = LoadIconW(0,IDI_APPLICATION);
    type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1); type.lpszClassName = L"MonitorFirmwareNativeWindow";
    if (!RegisterClassExW(&type)) { CloseHandle(app.decisionEvent); if (SUCCEEDED(ole)) OleUninitialize(); return 1; }
    HWND window = CreateWindowExW(0,type.lpszClassName,L"Samsung monitor firmware - MOCK development build",
        WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,980,760,0,0,instance,&app);
    if (!window) { CloseHandle(app.decisionEvent); if (SUCCEEDED(ole)) OleUninitialize(); return 1; }
    ShowWindow(window,app.smoke ? SW_HIDE : show); UpdateWindow(window);
    if (app.smoke) {
        SetWindowTextW(app.output,app.smokeRoot.c_str());
        if (!PostMessageW(window,SmokeNextMessage,0,0)) finishSmoke(app,false);
    }
    MSG message = {};
    while (GetMessageW(&message,0,0,0) > 0) {
        if (!IsDialogMessageW(window,&message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if (app.font) DeleteObject(app.font);
    if (app.titleFont) DeleteObject(app.titleFont);
    CloseHandle(app.decisionEvent);
    if (SUCCEEDED(ole)) OleUninitialize();
    return static_cast<int>(message.wParam);
}
