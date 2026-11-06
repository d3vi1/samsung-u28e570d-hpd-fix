#include "pch.h"
#include "MainPage.xaml.h"
using namespace MonitorFirmwareFrontend;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
App::App() { InitializeComponent(); }
void App::OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^) {
    Frame^ frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame == nullptr) {
        frame = ref new Frame();
        Window::Current->Content = frame;
    }
    if (frame->Content == nullptr) frame->Navigate(Windows::UI::Xaml::Interop::TypeName(MainPage::typeid));
    Window::Current->Activate();
}
