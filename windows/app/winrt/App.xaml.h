#pragma once
#include "App.g.h"
namespace MonitorFirmwareFrontend {
ref class App sealed {
public:
    App();
protected:
    virtual void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^ args) override;
};
}
