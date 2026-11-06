#pragma once
#include "MainPage.g.h"
namespace MonitorFirmwareFrontend {
public ref class MainPage sealed {
public:
    MainPage();
private:
    Windows::Storage::StorageFile^ firmware_;
    Windows::Storage::StorageFile^ evidence_;
    Windows::Storage::StorageFolder^ requestFolder_;
    Platform::String^ requestId_;
    Platform::String^ requestAction_;
    Platform::String^ requestMode_;
    Platform::String^ requestProvider_;
    bool busy_;
    bool pending_;
    bool terminal_;
    bool initialized_;
    void SetBusy(bool value);
    void Submit(Platform::String^ action);
    void RefreshResult();
    void ApplyResponse(Windows::Data::Json::JsonObject^ response);
    void ReportError(Platform::Exception^ error);
    void ContextChanged(Platform::Object^, Windows::UI::Xaml::Controls::SelectionChangedEventArgs^);
    void EndpointChanged(Platform::Object^, Windows::UI::Xaml::Controls::SelectionChangedEventArgs^);
    void EnumerateClick(Platform::Object^, Windows::UI::Xaml::RoutedEventArgs^);
    void FirmwareClick(Platform::Object^, Windows::UI::Xaml::RoutedEventArgs^);
    void ManifestClick(Platform::Object^, Windows::UI::Xaml::RoutedEventArgs^);
    void DownloadClick(Platform::Object^, Windows::UI::Xaml::RoutedEventArgs^);
    void UploadClick(Platform::Object^, Windows::UI::Xaml::RoutedEventArgs^);
    void VerifyClick(Platform::Object^, Windows::UI::Xaml::RoutedEventArgs^);
    void ResultClick(Platform::Object^, Windows::UI::Xaml::RoutedEventArgs^);
    void ExportClick(Platform::Object^, Windows::UI::Xaml::RoutedEventArgs^);
};
}
