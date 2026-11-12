#ifndef FLUTTER_PLUGIN_FLUTTER_TURBO_FETCH_PLUGIN_H_
#define FLUTTER_PLUGIN_FLUTTER_TURBO_FETCH_PLUGIN_H_

#ifndef TURBO_STANDALONE_TEST
#include <flutter_linux/flutter_linux.h>

G_BEGIN_DECLS

#ifdef FLUTTER_PLUGIN_IMPL
#define FLUTTER_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define FLUTTER_PLUGIN_EXPORT
#endif

G_DECLARE_FINAL_TYPE(FlutterTurboFetchPlugin, flutter_turbo_fetch_plugin,
                     FLUTTER, TURBO_FETCH_PLUGIN, GObject)

FLUTTER_PLUGIN_EXPORT FlutterTurboFetchPlugin* flutter_turbo_fetch_plugin_new(
    FlPluginRegistrar* registrar);

FLUTTER_PLUGIN_EXPORT void flutter_turbo_fetch_plugin_register_with_registrar(
    FlPluginRegistrar* registrar);

G_END_DECLS
#endif  // TURBO_STANDALONE_TEST

#endif  // FLUTTER_PLUGIN_FLUTTER_TURBO_FETCH_PLUGIN_H_
