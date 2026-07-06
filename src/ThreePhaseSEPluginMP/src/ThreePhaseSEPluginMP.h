#pragma once

#include <compiler/Definitions.h>
#include <sc/IPlugin.h>
#include <gui/LineEdit.h>
#include <functional>

#ifdef MU_WINDOWS
	#ifdef PLUGIN_EXPORTS
	#define PLUGIN_API __declspec(dllexport)
	#else
	#define PLUGIN_API __declspec(dllimport)
	#endif
#else
	#ifdef PLUGIN_EXPORTS
    #define PLUGIN_API __attribute__((visibility("default")))
	#else
    #define PLUGIN_API
	#endif
#endif

struct ConverterOptions
{
    td::String modelName;
    td::INT4 maxIter = 20;
    float tolerance = 0.000001f;

    // When true, the generated WLS model becomes a real state-estimation demo:
    // each voltage component gets several INDEPENDENT noisy measurements
    // (Gauss noise, std.dev = noiseDev), the estimator filters them back to the
    // true value, and the model reports the estimation-error statistics.
    // When false (default) the model reproduces the converted values exactly.
    bool addNoise = false;
    float noiseDev = 0.01f; // measurement noise standard deviation [p.u.]
};

using ProgressCallback = std::function<void(double)>;

void onClosedPluginWindow();

bool createModel(const td::String& inputFileName,
                 const td::String& outFileName,
                 sc::IPlugin* pIPlugin,
                 const ConverterOptions& options,
                 const ProgressCallback& onProgress,
                 td::String& status);
