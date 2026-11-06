#include "include/flutter_turbo_fetch/flutter_turbo_fetch_plugin_c_api.h"

#include <flutter/plugin_registrar_windows.h>

#include "flutter_turbo_fetch_plugin.h"

void FlutterTurboFetchPluginCApiRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  flutter_turbo_fetch::FlutterTurboFetchPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}

