#include "pch.h"
#include "MainPage.xaml.h"

using namespace MonitorFirmwareFrontend;
using namespace concurrency;
using namespace Platform;
using namespace Windows::Data::Json;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Storage;
using namespace Windows::Storage::Pickers;
using namespace Windows::Storage::Streams;
using namespace Windows::Security::Cryptography;
using namespace Windows::Security::Cryptography::Core;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;

namespace {
String^ Text(const std::wstring& value) { return ref new String(value.c_str()); }
String^ Number(double value) { std::wostringstream text; text << value; return Text(text.str()); }
bool Same(String^ a, String^ b) { return a != nullptr && b != nullptr && a->Equals(b); }
String^ LowerHex(IBuffer^ bytes) {
    std::wstring hex(CryptographicBuffer::EncodeToHexString(bytes)->Data());
    for (size_t i = 0; i < hex.size(); ++i) if (hex[i] >= L'A' && hex[i] <= L'F') hex[i] += L'a' - L'A';
    return Text(hex);
}
String^ SelectedTag(ComboBox^ box) {
    ComboBoxItem^ item = dynamic_cast<ComboBoxItem^>(box->SelectedItem);
    return item == nullptr ? L"" : dynamic_cast<String^>(item->Tag);
}
bool IsUuid(String^ value) {
    if (value == nullptr || value->Length() != 36) return false;
    for (unsigned i = 0; i < 36; ++i) {
        wchar_t c = value->Data()[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != L'-') return false; }
        else if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
    }
    return true;
}
String^ NewUuid() {
    Array<unsigned char>^ bytes;
    CryptographicBuffer::CopyToByteArray(CryptographicBuffer::GenerateRandom(16), &bytes);
    bytes[6] = (bytes[6] & 15) | 64;
    bytes[8] = (bytes[8] & 63) | 128;
    String^ hex = LowerHex(CryptographicBuffer::CreateFromByteArray(bytes));
    std::wstring raw(hex->Data());
    return Text(raw.substr(0, 8) + L"-" + raw.substr(8, 4) + L"-" + raw.substr(12, 4) + L"-" + raw.substr(16, 4) + L"-" + raw.substr(20));
}
void Put(JsonObject^ object, String^ key, String^ value) {
    object->Insert(key, JsonValue::CreateStringValue(value == nullptr ? L"" : value));
}
task<String^> ReadTextBounded(StorageFolder^ folder, String^ name) {
    return create_task(folder->GetFileAsync(name)).then([](StorageFile^ file) {
        return file->OpenReadAsync();
    }).then([](IRandomAccessStreamWithContentType^ stream) -> task<String^> {
        if (stream->Size > 1024 * 1024) { delete stream; throw ref new FailureException(L"Broker JSON exceeds 1 MiB."); }
        unsigned length = static_cast<unsigned>(stream->Size);
        DataReader^ reader = ref new DataReader(stream->GetInputStreamAt(0));
        reader->UnicodeEncoding = UnicodeEncoding::Utf8;
        return create_task(reader->LoadAsync(length)).then([reader, stream, length](task<unsigned> loaded) {
            try {
                if (loaded.get() != length) throw ref new FailureException(L"Incomplete broker JSON read.");
                String^ text = reader->ReadString(length);
                delete reader;
                delete stream;
                return text;
            } catch (...) { delete reader; delete stream; throw; }
        });
    });
}
task<void> CopyInput(StorageFile^ file, StorageFolder^ folder, String^ name, unsigned long long maximum) {
    if (file == nullptr) return create_task([] {});
    return create_task(file->GetBasicPropertiesAsync()).then([file, folder, name, maximum](Windows::Storage::FileProperties::BasicProperties^ properties) {
        if (properties->Size == 0 || properties->Size > maximum) throw ref new FailureException(L"Selected input has an unsupported size.");
        return file->CopyAsync(folder, name, NameCollisionOption::FailIfExists);
    }).then([maximum](StorageFile^ copied) {
        return copied->GetBasicPropertiesAsync();
    }).then([maximum](Windows::Storage::FileProperties::BasicProperties^ properties) {
        if (properties->Size == 0 || properties->Size > maximum) throw ref new FailureException(L"Copied input has an unsupported size.");
    });
}
task<String^> HashInput(StorageFolder^ folder) {
    return create_task(folder->GetFileAsync(L"input.bin")).then([](StorageFile^ file) {
        return file->OpenReadAsync();
    }).then([](IRandomAccessStreamWithContentType^ stream) -> task<String^> {
        if (stream->Size == 0 || stream->Size > 0x200000) { delete stream; throw ref new FailureException(L"Input exceeds the 2 MiB image limit."); }
        unsigned length = static_cast<unsigned>(stream->Size);
        DataReader^ reader = ref new DataReader(stream->GetInputStreamAt(0));
        return create_task(reader->LoadAsync(length)).then([reader, stream, length](task<unsigned> loaded) {
            try {
                if (loaded.get() != length) throw ref new FailureException(L"Incomplete input read.");
                IBuffer^ bytes = reader->ReadBuffer(length);
                String^ hash = LowerHex(HashAlgorithmProvider::OpenAlgorithm(HashAlgorithmNames::Sha256)->HashData(bytes));
                delete reader;
                delete stream;
                return hash;
            } catch (...) { delete reader; delete stream; throw; }
        });
    });
}
task<void> PublishRequest(StorageFolder^ folder, JsonObject^ request) {
    return create_task(folder->CreateFileAsync(L"request.pending", CreationCollisionOption::FailIfExists)).then([request](StorageFile^ file) {
        return create_task(FileIO::WriteTextAsync(file, request->Stringify(), UnicodeEncoding::Utf8)).then([file] {
            // The broker is launched only after the complete request becomes visible.
            return file->RenameAsync(L"request.json", NameCollisionOption::FailIfExists);
        });
    });
}
struct ExportBudget { unsigned files; unsigned long long bytes; ExportBudget() : files(0), bytes(0) {} };
task<void> CopyTree(StorageFolder^ from, StorageFolder^ to, unsigned depth, std::shared_ptr<ExportBudget> budget) {
    if (depth > 8) throw ref new FailureException(L"Artifact directory nesting exceeds the export limit.");
    return create_task(from->GetItemsAsync()).then([to, depth, budget](IVectorView<IStorageItem^>^ items) {
        if (items->Size > 256) throw ref new FailureException(L"Too many artifacts in one directory.");
        task<void> chain = create_task([] {});
        for (unsigned i = 0; i < items->Size; ++i) {
            IStorageItem^ item = items->GetAt(i);
            chain = chain.then([to, depth, budget, item]() -> task<void> {
                if (++budget->files > 1024) throw ref new FailureException(L"Artifact export exceeds 1024 entries.");
                if (item->IsOfType(StorageItemTypes::Folder)) {
                    StorageFolder^ child = safe_cast<StorageFolder^>(item);
                    return create_task(to->CreateFolderAsync(child->Name, CreationCollisionOption::FailIfExists)).then([child, depth, budget](StorageFolder^ target) {
                        return CopyTree(child, target, depth + 1, budget);
                    });
                }
                StorageFile^ file = safe_cast<StorageFile^>(item);
                return create_task(file->GetBasicPropertiesAsync()).then([file, to, budget](Windows::Storage::FileProperties::BasicProperties^ properties) {
                    if (properties->Size > 64 * 1024 * 1024 || budget->bytes > 64 * 1024 * 1024 - properties->Size)
                        throw ref new FailureException(L"Artifact export exceeds 64 MiB.");
                    budget->bytes += properties->Size;
                    return file->CopyAsync(to, file->Name, NameCollisionOption::FailIfExists);
                }).then([](StorageFile^) {});
            });
        }
        return chain;
    });
}
}

MainPage::MainPage() : busy_(false), pending_(false), terminal_(false), initialized_(false) {
    InitializeComponent();
    initialized_ = true;
    auto values = ApplicationData::Current->LocalSettings->Values;
    if (values->HasKey(L"request_id")) {
        String^ saved = dynamic_cast<String^>(values->Lookup(L"request_id"));
        if (IsUuid(saved)) {
            requestId_ = saved;
            pending_ = true;
            RequestText->Text = L"Previous request: " + requestId_;
            StatusText->Text = L"Refresh the previous request before starting another operation. No action is automatically relaunched.";
            if (values->HasKey(L"request_action")) requestAction_ = dynamic_cast<String^>(values->Lookup(L"request_action"));
            if (values->HasKey(L"request_mode")) requestMode_ = dynamic_cast<String^>(values->Lookup(L"request_mode"));
            if (values->HasKey(L"request_provider")) requestProvider_ = dynamic_cast<String^>(values->Lookup(L"request_provider"));
            ModeBox->SelectedIndex = Same(requestMode_, L"hardware") ? 1 : 0;
        }
    }
    SetBusy(false);
}
void MainPage::SetBusy(bool value) {
    busy_ = value;
    bool available = !busy_ && !pending_;
    ModeBox->IsEnabled = ProviderBox->IsEnabled = ImageKindBox->IsEnabled = available;
    FirmwareButton->IsEnabled = ManifestButton->IsEnabled = EnumerateButton->IsEnabled = available;
    EndpointBox->IsEnabled = available;
    bool selected = EndpointBox->SelectedItem != nullptr;
    DownloadButton->IsEnabled = UploadButton->IsEnabled = VerifyButton->IsEnabled = available && selected;
    ResultButton->IsEnabled = !busy_ && requestId_ != nullptr;
    ExportButton->IsEnabled = !busy_ && terminal_ && requestFolder_ != nullptr;
}
void MainPage::ReportError(Exception^ error) { StatusText->Text = L"Error: " + error->Message; SetBusy(false); }
void MainPage::ContextChanged(Object^, SelectionChangedEventArgs^) {
    if (!initialized_) return;
    EndpointBox->Items->Clear();
    EndpointDetails->Text = L"Refresh endpoints for this mode and provider.";
    SetBusy(busy_);
}
void MainPage::EndpointChanged(Object^, SelectionChangedEventArgs^) {
    if (!initialized_) return;
    ComboBoxItem^ item = dynamic_cast<ComboBoxItem^>(EndpointBox->SelectedItem);
    if (item != nullptr) {
        JsonObject^ endpoint = safe_cast<JsonObject^>(item->Tag);
        EndpointDetails->Text = endpoint->Stringify();
    }
    SetBusy(busy_);
}
void MainPage::EnumerateClick(Object^, RoutedEventArgs^) { Submit(L"enumerate"); }
void MainPage::DownloadClick(Object^, RoutedEventArgs^) { Submit(L"download"); }
void MainPage::UploadClick(Object^, RoutedEventArgs^) { Submit(L"upload"); }
void MainPage::VerifyClick(Object^, RoutedEventArgs^) { Submit(L"verify"); }
void MainPage::ResultClick(Object^, RoutedEventArgs^) { RefreshResult(); }

void MainPage::FirmwareClick(Object^, RoutedEventArgs^) {
    SetBusy(true);
    try {
    FileOpenPicker^ picker = ref new FileOpenPicker();
    picker->FileTypeFilter->Append(L".bin");
    picker->SuggestedStartLocation = PickerLocationId::DocumentsLibrary;
    create_task(picker->PickSingleFileAsync()).then([this](task<StorageFile^> picked) {
        try { StorageFile^ file = picked.get(); if (file != nullptr) { firmware_ = file; FirmwareName->Text = file->Name; } SetBusy(false); }
        catch (Exception^ error) { ReportError(error); }
    }, task_continuation_context::use_current());
    } catch (Exception^ error) { ReportError(error); }
}
void MainPage::ManifestClick(Object^, RoutedEventArgs^) {
    SetBusy(true);
    try {
    FileOpenPicker^ picker = ref new FileOpenPicker();
    picker->FileTypeFilter->Append(L".json");
    picker->SuggestedStartLocation = PickerLocationId::DocumentsLibrary;
    create_task(picker->PickSingleFileAsync()).then([this](task<StorageFile^> picked) {
        try { StorageFile^ file = picked.get(); if (file != nullptr) { evidence_ = file; ManifestName->Text = file->Name; } SetBusy(false); }
        catch (Exception^ error) { ReportError(error); }
    }, task_continuation_context::use_current());
    } catch (Exception^ error) { ReportError(error); }
}

void MainPage::Submit(String^ action) {
    if (busy_ || pending_) return;
    try {
        bool inputRequired = Same(action, L"upload") || Same(action, L"verify");
        String^ kind = inputRequired ? SelectedTag(ImageKindBox) : L"samsung_package";
        if (inputRequired && firmware_ == nullptr) throw ref new FailureException(L"Choose a firmware file first.");
        if (inputRequired && Same(kind, L"spi_full") && evidence_ == nullptr) throw ref new FailureException(L"Choose the backup evidence for your own SPI dump.");
        JsonObject^ request = ref new JsonObject();
        String^ id = NewUuid();
        Put(request, L"schema", L"monitorfw.request.v1"); Put(request, L"request_id", id);
        long long utc = (ref new Windows::Globalization::Calendar())->GetDateTime().UniversalTime;
        request->Insert(L"created_unix", JsonValue::CreateNumberValue(static_cast<double>((utc - 116444736000000000LL) / 10000000)));
        Put(request, L"action", action); Put(request, L"mode", SelectedTag(ModeBox)); Put(request, L"provider_id", SelectedTag(ProviderBox));
        Put(request, L"image_kind", kind); Put(request, L"firmware_sha256", L"");
        Put(request, L"endpoint_locator", L""); Put(request, L"expected_edid_sha256", L"");
        if (!Same(action, L"enumerate")) {
            ComboBoxItem^ item = dynamic_cast<ComboBoxItem^>(EndpointBox->SelectedItem);
            if (item == nullptr) throw ref new FailureException(L"Refresh and select an endpoint first.");
            JsonObject^ endpoint = safe_cast<JsonObject^>(item->Tag);
            Put(request, L"endpoint_locator", endpoint->GetNamedString(L"locator"));
            Put(request, L"expected_edid_sha256", endpoint->GetNamedString(L"edid_sha256"));
        }
        StorageFile^ input = inputRequired ? firmware_ : nullptr;
        StorageFile^ evidence = inputRequired && Same(kind, L"spi_full") ? evidence_ : nullptr;
        SetBusy(true); terminal_ = false; Progress->IsIndeterminate = false; Progress->Value = 0; ResultText->Text = L"";
        StatusText->Text = L"Preparing local request files...";
        create_task(ApplicationData::Current->LocalFolder->CreateFolderAsync(L"requests", CreationCollisionOption::OpenIfExists)).then([id](StorageFolder^ requests) {
            return requests->CreateFolderAsync(id, CreationCollisionOption::FailIfExists);
        }).then([this, request, input, evidence](StorageFolder^ folder) {
            requestFolder_ = folder;
            return CopyInput(input, folder, L"input.bin", 0x200000).then([folder, evidence] {
                return CopyInput(evidence, folder, L"backup-evidence.json", 65536);
            }).then([folder, input]() -> task<String^> {
                if (input == nullptr) return create_task([]() -> String^ { return L""; });
                return HashInput(folder);
            }).then([request, folder](String^ hash) {
                Put(request, L"firmware_sha256", hash);
                return PublishRequest(folder, request);
            });
        }).then([this, id, action, request] {
            requestId_ = id; requestAction_ = action;
            requestMode_ = request->GetNamedString(L"mode"); requestProvider_ = request->GetNamedString(L"provider_id");
            auto values = ApplicationData::Current->LocalSettings->Values;
            values->Insert(L"request_id", requestId_); values->Insert(L"request_action", requestAction_);
            values->Insert(L"request_mode", requestMode_); values->Insert(L"request_provider", requestProvider_);
            pending_ = true;
            RequestText->Text = L"Request: " + id;
            StatusText->Text = L"Opening the desktop bridge. Returning to this app does not mean the operation has finished.";
            return Windows::System::Launcher::LaunchUriAsync(ref new Uri(L"monitorfw-bridge://request/" + id));
        }, task_continuation_context::use_current()).then([this](task<bool> launched) {
            try {
                if (launched.get()) StatusText->Text = L"Bridge launch accepted. Return here and refresh the result; upload confirmation appears on the desktop.";
                else {
                    pending_ = false;
                    // A false launch result is not a dispatched request. Do not
                    // resurrect it as pending after the frontend is reopened.
                    auto values = ApplicationData::Current->LocalSettings->Values;
                    if (values->HasKey(L"request_id")) values->Remove(L"request_id");
                    if (values->HasKey(L"request_action")) values->Remove(L"request_action");
                    if (values->HasKey(L"request_mode")) values->Remove(L"request_mode");
                    if (values->HasKey(L"request_provider")) values->Remove(L"request_provider");
                    StatusText->Text = L"The bridge was not launched. Install/configure the matching desktop bridge, then submit a new request. Nothing was automatically retried.";
                }
                SetBusy(false);
            } catch (Exception^ error) { ReportError(error); }
        }, task_continuation_context::use_current());
    } catch (Exception^ error) { ReportError(error); }
}

void MainPage::ApplyResponse(JsonObject^ response) {
    if (!Same(response->GetNamedString(L"schema"), L"monitorfw.response.v1") || !Same(response->GetNamedString(L"request_id"), requestId_))
        throw ref new FailureException(L"The result does not match this request.");
    bool success = response->GetNamedBoolean(L"success");
    String^ responseMode = response->GetNamedString(L"mode");
    String^ responseProvider = response->GetNamedString(L"provider_id");
    if (!Same(responseMode, L"unknown") && (!Same(responseMode, requestMode_) || !Same(responseProvider, requestProvider_)))
        throw ref new FailureException(L"The result mode/provider does not match this request.");
    if (success && Same(responseMode, L"unknown")) throw ref new FailureException(L"A successful result must identify its mode.");
    String^ error = response->GetNamedString(L"error");
    JsonArray^ providers = response->GetNamedArray(L"providers");
    JsonArray^ endpoints = response->GetNamedArray(L"endpoints");
    if (providers->Size > 16 || endpoints->Size > 256) throw ref new FailureException(L"Unexpected provider or endpoint count.");
    if (!Same(response->GetNamedString(L"artifacts_directory"), L"artifacts")) throw ref new FailureException(L"Unexpected artifacts directory.");
    std::wstring summary;
    for (unsigned i = 0; i < providers->Size; ++i) {
        JsonObject^ provider = providers->GetObjectAt(i);
        summary += provider->GetNamedString(L"name")->Data();
        summary += provider->GetNamedBoolean(L"implemented") ? L": implemented. " : L": unavailable. ";
        summary += provider->GetNamedString(L"reason")->Data(); summary += L"\n";
    }
    if (!summary.empty()) ProviderDetails->Text = Text(summary);
    if (Same(requestAction_, L"enumerate") && success && Same(SelectedTag(ModeBox), requestMode_) && Same(SelectedTag(ProviderBox), requestProvider_)) {
        EndpointBox->Items->Clear();
        for (unsigned i = 0; i < endpoints->Size; ++i) {
            JsonObject^ endpoint = endpoints->GetObjectAt(i);
            if (!Same(endpoint->GetNamedString(L"provider_id"), requestProvider_)) continue;
            ComboBoxItem^ item = ref new ComboBoxItem();
            item->Content = endpoint->GetNamedString(L"model") + L" / " + endpoint->GetNamedString(L"serial") + (endpoint->GetNamedBoolean(L"ambiguous") ? L" / ambiguous identity" : L"");
            item->Tag = endpoint;
            EndpointBox->Items->Append(item);
        }
        // Selection is deliberate, even when only one endpoint is returned.
        EndpointDetails->Text = L"Select an endpoint from the refreshed list.";
    }
    ResultText->Text = response->Stringify();
    String^ modeLabel = Same(responseMode, L"mock") ? L"MOCK / simulated" : (Same(responseMode, L"hardware") ? L"HARDWARE" : L"UNKNOWN / request rejected");
    StatusText->Text = modeLabel + (success ? L": the desktop bridge reported completion. Review and export the result and artifacts." : L": the desktop bridge reported failure: " + error);
    Progress->IsIndeterminate = false;
    pending_ = false; terminal_ = true;
    SetBusy(false);
}
void MainPage::RefreshResult() {
    if (busy_ || !IsUuid(requestId_)) return;
    SetBusy(true);
    create_task(ApplicationData::Current->LocalFolder->GetFolderAsync(L"requests")).then([this](StorageFolder^ requests) {
        return requests->GetFolderAsync(requestId_);
    }).then([this](StorageFolder^ folder) {
        requestFolder_ = folder;
        return ReadTextBounded(folder, L"response.json");
    }).then([this](task<String^> response) -> task<void> {
        try {
            ApplyResponse(JsonObject::Parse(response.get()));
            return create_task([] {});
        } catch (Exception^ error) {
            // Missing final response is normal while the desktop broker works.
            // Other errors are not treated as evidence that a request finished.
            if (error->HResult != static_cast<int>(0x80070002)) throw;
            return ReadTextBounded(requestFolder_, L"progress.json").then([this](task<String^> progress) {
                try {
                    JsonObject^ value = JsonObject::Parse(progress.get());
                    double done = value->GetNamedNumber(L"done"), total = value->GetNamedNumber(L"total");
                    if (!(done >= 0 && total >= 0) || (total > 0 && done > total)) throw ref new FailureException(L"Invalid progress values.");
                    Progress->IsIndeterminate = total == 0;
                    Progress->Maximum = total > 0 ? total : 1; Progress->Value = total > 0 ? done : 0;
                    ProgressText->Text = value->GetNamedString(L"phase") + L" - " + Number(done) + L" / " + Number(total);
                    StatusText->Text = L"Progress snapshot received. A final response is still required.";
                } catch (Exception^ error) {
                    if (error->HResult != static_cast<int>(0x80070002)) throw;
                    StatusText->Text = L"No response or progress yet. The desktop bridge may be waiting for confirmation. This app does not retry the operation.";
                }
            }, task_continuation_context::use_current());
        }
    }, task_continuation_context::use_current()).then([this](task<void> completed) {
        try { completed.get(); SetBusy(false); } catch (Exception^ error) { ReportError(error); }
    }, task_continuation_context::use_current());
}
void MainPage::ExportClick(Object^, RoutedEventArgs^) {
    if (busy_ || !terminal_ || requestFolder_ == nullptr) return;
    SetBusy(true);
    try {
    FolderPicker^ picker = ref new FolderPicker();
    picker->FileTypeFilter->Append(L"*");
    picker->SuggestedStartLocation = PickerLocationId::DocumentsLibrary;
    std::shared_ptr<bool> exported = std::make_shared<bool>(false);
    create_task(picker->PickSingleFolderAsync()).then([this, exported](StorageFolder^ selected) -> task<void> {
        if (selected == nullptr) return create_task([] {});
        StorageFolder^ source = requestFolder_;
        String^ name = L"monitorfw-" + requestId_;
        return create_task(selected->CreateFolderAsync(name, CreationCollisionOption::FailIfExists)).then([source, exported](StorageFolder^ target) {
            return create_task(source->GetFolderAsync(L"artifacts")).then([target](task<StorageFolder^> found) -> task<void> {
                StorageFolder^ artifacts;
                try { artifacts = found.get(); }
                catch (Exception^ error) {
                    if (error->HResult != static_cast<int>(0x80070002)) throw;
                    return create_task([] {});
                }
                return create_task(target->CreateFolderAsync(L"artifacts", CreationCollisionOption::FailIfExists)).then([artifacts](StorageFolder^ output) {
                    return CopyTree(artifacts, output, 0, std::make_shared<ExportBudget>());
                });
            }).then([source, target] {
                return create_task(source->GetFileAsync(L"response.json")).then([target](StorageFile^ response) {
                    return response->CopyAsync(target, L"response.json", NameCollisionOption::FailIfExists);
                });
            }).then([exported](StorageFile^) { *exported = true; });
        });
    }, task_continuation_context::use_current()).then([this, exported](task<void> finished) {
        try {
            finished.get();
            if (*exported) StatusText->Text = L"Artifacts and final response exported. Existing files were not overwritten.";
            SetBusy(false);
        } catch (Exception^ error) { ReportError(error); }
    }, task_continuation_context::use_current());
    } catch (Exception^ error) { ReportError(error); }
}
