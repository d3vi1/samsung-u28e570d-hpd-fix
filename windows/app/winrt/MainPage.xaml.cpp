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
bool IsAsciiAlphanumeric(wchar_t c) {
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9');
}
std::wstring LowerAscii(String^ value) {
    std::wstring result(value->Data());
    for (size_t i = 0; i < result.size(); ++i) if (result[i] >= L'A' && result[i] <= L'Z') result[i] += L'a' - L'A';
    return result;
}
bool IsBackupLeaf(String^ value) {
    if (value == nullptr || value->Length() == 0 || value->Length() > 255) return false;
    std::wstring leaf = LowerAscii(value);
    if (!IsAsciiAlphanumeric(leaf[0]) || leaf[leaf.size() - 1] == L'.') return false;
    for (size_t i = 0; i < leaf.size(); ++i)
        if (!IsAsciiAlphanumeric(leaf[i]) && leaf[i] != L'.' && leaf[i] != L'_' && leaf[i] != L'-') return false;
    std::wstring stem = leaf.substr(0, leaf.find(L'.'));
    if (stem == L"con" || stem == L"prn" || stem == L"aux" || stem == L"nul") return false;
    if (stem.size() == 4 && (stem.substr(0, 3) == L"com" || stem.substr(0, 3) == L"lpt") && stem[3] >= L'1' && stem[3] <= L'9') return false;
    return true;
}
bool IsSha256(String^ value) {
    if (value == nullptr || value->Length() != 64) return false;
    for (unsigned i = 0; i < value->Length(); ++i) {
        wchar_t c = value->Data()[i];
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
    }
    return true;
}
void ValidateEvidenceReferences(JsonObject^ manifest) {
    if (!Same(manifest->GetNamedString(L"schema"), L"monitorfw.backup.v1") || !Same(manifest->GetNamedString(L"image_kind"), L"spi_full"))
        throw ref new FailureException(L"The selected folder does not contain a full SPI backup manifest.");
    JsonArray^ reads = manifest->GetNamedArray(L"reads");
    JsonObject^ flash = manifest->GetNamedObject(L"flash");
    if (reads->Size != 2 || !manifest->GetNamedBoolean(L"reads_equal")) throw ref new FailureException(L"Two matching backup reads are required.");
    JsonObject^ first = reads->GetObjectAt(0);
    JsonObject^ second = reads->GetObjectAt(1);
    String^ firstName = first->GetNamedString(L"file");
    String^ secondName = second->GetNamedString(L"file");
    String^ sfdpName = flash->GetNamedString(L"sfdp_file");
    if (!IsBackupLeaf(firstName) || !IsBackupLeaf(secondName) || !IsBackupLeaf(sfdpName))
        throw ref new FailureException(L"Backup references must be safe file names without paths.");
    if (LowerAscii(firstName) == LowerAscii(secondName) || LowerAscii(firstName) == LowerAscii(sfdpName) || LowerAscii(secondName) == LowerAscii(sfdpName))
        throw ref new FailureException(L"Both read files and the SFDP file must have distinct names, ignoring case.");
    if (first->GetNamedNumber(L"bytes") != 0x200000 || second->GetNamedNumber(L"bytes") != 0x200000 || flash->GetNamedNumber(L"sfdp_bytes") != 64)
        throw ref new FailureException(L"Each SPI read must be 2 MiB and the SFDP capture must be 64 bytes.");
    if (!IsSha256(first->GetNamedString(L"sha256")) || !Same(first->GetNamedString(L"sha256"), second->GetNamedString(L"sha256")) || !IsSha256(flash->GetNamedString(L"sfdp_sha256")))
        throw ref new FailureException(L"Backup hashes must be lowercase SHA256 values and both read hashes must agree.");
    // This checks references needed for copying. The broker independently validates
    // the complete manifest, actual bytes, identity, flash profile and policy.
}
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
task<String^> ReadTextBounded(StorageFolder^ folder, String^ name, unsigned maximum = 1024 * 1024) {
    return create_task(folder->GetFileAsync(name)).then([](StorageFile^ file) {
        return file->OpenReadAsync();
    }).then([maximum](IRandomAccessStreamWithContentType^ stream) -> task<String^> {
        if (stream->Size == 0 || stream->Size > maximum) { delete stream; throw ref new FailureException(L"JSON file has an unsupported size."); }
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
task<String^> HashCopiedFile(StorageFolder^ folder, String^ name, unsigned exactSize) {
    return create_task(folder->GetFileAsync(name)).then([](StorageFile^ file) {
        return file->OpenReadAsync();
    }).then([exactSize](IRandomAccessStreamWithContentType^ stream) -> task<String^> {
        if (stream->Size == 0 || stream->Size > 0x200000) { delete stream; throw ref new FailureException(L"Input exceeds the 2 MiB image limit."); }
        if (exactSize != 0 && stream->Size != exactSize) { delete stream; throw ref new FailureException(L"Copied evidence has an unexpected length."); }
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
task<void> CopyEvidenceFile(StorageFolder^ source, StorageFolder^ destination, String^ name, unsigned exactSize, String^ expectedHash) {
    return create_task(source->GetFileAsync(name)).then([destination, name, exactSize](StorageFile^ file) {
        return CopyInput(file, destination, name, exactSize);
    }).then([destination, name, exactSize] {
        return HashCopiedFile(destination, name, exactSize);
    }).then([expectedHash](String^ hash) {
        if (!Same(hash, expectedHash)) throw ref new FailureException(L"Copied backup evidence does not match its manifest hash.");
    });
}
task<String^> CopyEvidenceBundle(StorageFolder^ source, StorageFolder^ destination) {
    if (source == nullptr) return create_task([]() -> String^ { return L""; });
    return create_task(source->GetFileAsync(L"backup.json")).then([destination](StorageFile^ manifest) {
        return CopyInput(manifest, destination, L"backup-evidence.json", 65536);
    }).then([destination] {
        return ReadTextBounded(destination, L"backup-evidence.json", 65536);
    }).then([source, destination](String^ text) {
        JsonObject^ manifest = JsonObject::Parse(text);
        ValidateEvidenceReferences(manifest);
        return create_task(destination->CreateFolderAsync(L"evidence", CreationCollisionOption::FailIfExists)).then([source, manifest](StorageFolder^ evidence) {
            JsonArray^ reads = manifest->GetNamedArray(L"reads");
            JsonObject^ first = reads->GetObjectAt(0);
            JsonObject^ second = reads->GetObjectAt(1);
            JsonObject^ flash = manifest->GetNamedObject(L"flash");
            return CopyEvidenceFile(source, evidence, first->GetNamedString(L"file"), 0x200000, first->GetNamedString(L"sha256")).then([source, evidence, second] {
                return CopyEvidenceFile(source, evidence, second->GetNamedString(L"file"), 0x200000, second->GetNamedString(L"sha256"));
            }).then([source, evidence, flash] {
                return CopyEvidenceFile(source, evidence, flash->GetNamedString(L"sfdp_file"), 64, flash->GetNamedString(L"sfdp_sha256"));
            }).then([first]() -> String^ { return first->GetNamedString(L"sha256"); });
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
    ModeBox->IsEnabled = available;
    ProviderBox->IsEnabled = available;
    ImageKindBox->IsEnabled = available;
    FirmwareButton->IsEnabled = available;
    ManifestButton->IsEnabled = available;
    EnumerateButton->IsEnabled = available;
    EndpointBox->IsEnabled = available;
    bool selected = EndpointBox->SelectedItem != nullptr;
    DownloadButton->IsEnabled = available && selected;
    UploadButton->IsEnabled = available && selected;
    VerifyButton->IsEnabled = available && selected;
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
    FolderPicker^ picker = ref new FolderPicker();
    picker->FileTypeFilter->Append(L"*");
    picker->SuggestedStartLocation = PickerLocationId::DocumentsLibrary;
    create_task(picker->PickSingleFolderAsync()).then([this](StorageFolder^ folder) -> task<void> {
        if (folder == nullptr) return create_task([] {});
        return ReadTextBounded(folder, L"backup.json", 65536).then([this, folder](String^ text) {
            ValidateEvidenceReferences(JsonObject::Parse(text));
            evidenceFolder_ = folder;
            ManifestName->Text = folder->Name + L" / backup.json. All three evidence files will be copied and checked before dispatch.";
        }, task_continuation_context::use_current());
    }, task_continuation_context::use_current()).then([this](task<void> selected) {
        try { selected.get(); SetBusy(false); }
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
        if (inputRequired && Same(kind, L"spi_full") && evidenceFolder_ == nullptr) throw ref new FailureException(L"Choose the bundle folder containing backup.json and all three evidence files.");
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
        StorageFolder^ evidence = inputRequired && Same(kind, L"spi_full") ? evidenceFolder_ : nullptr;
        SetBusy(true); terminal_ = false; Progress->IsIndeterminate = false; Progress->Value = 0; ResultText->Text = L"";
        StatusText->Text = L"Preparing local request files...";
        create_task(ApplicationData::Current->LocalFolder->CreateFolderAsync(L"requests", CreationCollisionOption::OpenIfExists)).then([id](StorageFolder^ requests) {
            return requests->CreateFolderAsync(id, CreationCollisionOption::FailIfExists);
        }).then([this, request, input, evidence](StorageFolder^ folder) {
            requestFolder_ = folder;
            return CopyInput(input, folder, L"input.bin", 0x200000).then([folder, evidence] {
                return CopyEvidenceBundle(evidence, folder);
            }).then([folder, input](String^ evidenceHash) -> task<String^> {
                if (input == nullptr) return create_task([]() -> String^ { return L""; });
                return HashCopiedFile(folder, L"input.bin", evidenceHash->Length() == 0 ? 0 : 0x200000).then([evidenceHash](String^ hash) {
                    if (evidenceHash->Length() != 0 && !Same(hash, evidenceHash)) throw ref new FailureException(L"The selected SPI image differs from the two backup reads.");
                    return hash;
                });
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
