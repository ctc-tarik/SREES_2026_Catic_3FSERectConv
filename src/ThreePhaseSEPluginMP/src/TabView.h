#pragma once

#include <gui/StandardTabView.h>
#include "ViewConv.h"

class TabView : public gui::StandardTabView
{
protected:
    ViewConv _viewConv;

    TabView() = delete;

public:
    TabView(sc::IPlugin* pIPlugin, const sc::IPlugin::CallBack& onComplete)
    : _viewConv(pIPlugin, onComplete)
    {
        addView(&_viewConv, tr("Converter"));
    }

    td::String getOutFileName() const
    {
        return _viewConv.getOutFileName();
    }
};
