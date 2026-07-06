#pragma once

#include <gui/View.h>
#include <gui/Label.h>
#include <gui/Button.h>
#include <gui/LineEdit.h>
#include <gui/TextEdit.h>
#include <gui/CheckBox.h>
#include <gui/NumericEdit.h>
#include <gui/ProgressIndicator.h>
#include <gui/GridLayout.h>
#include <gui/GridComposer.h>
#include <gui/HorizontalLayout.h>
#include <gui/FileDialog.h>
#include <fo/FileOperations.h>
#include <atomic>
#include <cstdio>
#include <thread>
#include "ThreePhaseSEPluginMP.h"

class ViewConv : public gui::View
{
protected:
    sc::IPlugin* _pIPlugin;
    sc::IPlugin::CallBack _onComplete;

    gui::Label _lblSectionIO;
    gui::Label _lblFnIn;
    gui::LineEdit _editFnIn;
    gui::Label _lblFnOut;
    gui::LineEdit _editFnOut;

    gui::Label _lblSectionOptions;
    gui::Label _lblModelName;
    gui::LineEdit _editModelName;
    gui::CheckBox _chkNoise;
    gui::Label _lblNoiseDev;
    gui::NumericEdit _neNoiseDev;

    gui::Label _lblSectionProgress;
    gui::Label _lblProgress;
    gui::ProgressIndicator _progIndicator;
    gui::LineEdit _editProgress;
    gui::Label _lblStatus;
    gui::LineEdit _editStatus;

    gui::Label _lblSectionInfo;
    gui::TextEdit _teInfo;

    gui::Button _btnSelectInFn;
    gui::Button _btnSelectOutFn;
    gui::Button _btnInfo;
    gui::Button _btnConvert;
    gui::HorizontalLayout _hlButtons;
    gui::GridLayout _gl;

    td::UINT4 _wndID = 12000;
    std::atomic_bool _working { false };
    std::atomic_bool _closing { false };

    // Background conversion driver: keeps the GUI thread free to repaint
    // (e.g. the progress bar) while createModel() runs its own worker +
    // progress threads. Progress/completion are marshaled back to the GUI
    // thread via gui::NatObject::asyncCall, never touched directly from here.
    std::thread _convThread;
    gui::AsyncFn _asyncUpdateProgress;
    gui::AsyncFn _asyncOnConversionDone;
    std::atomic<double> _progressValue { 0.0 };
    bool _lastOk = false;
    td::String _lastStatus;

protected:
    ConverterOptions getOptions()
    {
        ConverterOptions options;
        options.modelName = _editModelName.getText();
        options.maxIter = 20;
        options.tolerance = 0.000001f;
        options.addNoise = _chkNoise.isChecked();
        options.noiseDev = _neNoiseDev.getValue().r4Val();
        return options;
    }

    void updateProgressUI()
    {
        if (_closing)
            return;
        double p = _progressValue.load();
        _progIndicator.setValue(p);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.0f%%", p * 100.0);
        _editProgress = buf;
    }

    void onConversionDone()
    {
        if (_closing)
            return;
        _working = false;
        _btnConvert.enable(true);
        _editStatus = _lastStatus;
        if (_lastOk)
        {
            _progressValue.store(1.0);
            _progIndicator.setValue(1.0);
            _editProgress = "100%";
        }
    }

    // Runs on _convThread. createModel() itself spawns the two required
    // threads (conversion worker + real-time progress reporter); this
    // function only keeps that pair off the GUI thread and relays results.
    void runConversion(td::String inputFileName, td::String outFileName, ConverterOptions options)
    {
        auto onProgress = [this](double progress) {
            if (_closing)
                return;
            _progressValue.store(progress);
            asyncCall(&_asyncUpdateProgress);
        };

        td::String status;
        bool ok = createModel(inputFileName, outFileName, _pIPlugin, options, onProgress, status);

        if (_closing)
            return;
        _lastOk = ok;
        _lastStatus = status;
        asyncCall(&_asyncOnConversionDone);
    }

    void handleUserActions()
    {
        _btnSelectInFn.onClick([this]{
            gui::OpenFileDialog::show(this, tr("OpenInputModel"), "*.m", _wndID + 1000, [this](gui::FileDialog* pDlg)
            {
                if (pDlg->getStatus() == gui::FileDialog::Status::OK)
                {
                    td::String fileName = pDlg->getFileName();
                    if (!fileName.isEmpty())
                        _editFnIn = fileName;
                }
            });
        });

        _btnSelectOutFn.onClick([this]{
            gui::SaveFileDialog::show(this, tr("CreateDigitalModel"), "*.dmodl", _wndID + 2000, [this](gui::FileDialog* pDlg)
            {
                if (pDlg->getStatus() == gui::FileDialog::Status::OK)
                {
                    td::String fileName = pDlg->getFileName();
                    if (!fileName.isEmpty())
                        _editFnOut = fileName;
                }
            });
        });

        _btnInfo.onClick([this]{
            td::String fileName = _editFnIn.getText();
            if (!fo::fileExists(fileName))
            {
                _editStatus = "ERROR! Input file does not exist.";
                return;
            }
            td::String content;
            if (!fo::loadBinaryFile(fileName, content))
            {
                _editStatus = "ERROR! Cannot load input file.";
                return;
            }
            _teInfo.setText(content);
            _editStatus = "INFO! Input file loaded.";
        });

        _btnConvert.onClick([this]{
            if (_working)
            {
                _editStatus = "INFO! Conversion is already running.";
                return;
            }

            td::String inputFileName = _editFnIn.getText();
            td::String outFileName = _editFnOut.getText();
            if (inputFileName.isEmpty() || !fo::fileExists(inputFileName))
            {
                _editStatus = "ERROR! Select a valid input file (.m).";
                return;
            }
            if (outFileName.isEmpty())
            {
                _editStatus = "ERROR! Select output .dmodl file.";
                return;
            }

            if (_convThread.joinable())
                _convThread.join(); // previous run already finished; reclaim the thread object

            _working = true;
            _btnConvert.enable(false);
            _progressValue.store(0.0);
            _progIndicator.setValue(0.0);
            _editProgress = "0%";
            _editStatus = "INFO! Converting...";

            auto options = getOptions();
            _convThread = std::thread(&ViewConv::runConversion, this, inputFileName, outFileName, options);
        });
    }

public:
    ViewConv(sc::IPlugin* pIPlugin, const sc::IPlugin::CallBack& onComplete)
    : _pIPlugin(pIPlugin)
    , _onComplete(onComplete)
    , _lblSectionIO(tr("Input / Output"), gui::Font::ID::SystemBold)
    , _lblFnIn(tr("Input file (.m):"))
    , _lblFnOut(tr("Output file:"))
    , _lblSectionOptions(tr("Conversion options"), gui::Font::ID::SystemBold)
    , _lblModelName(tr("Model name:"))
    , _chkNoise(tr("Add measurement noise (WLS estimation demo):"))
    , _lblNoiseDev(tr("Noise std.dev [p.u.]:"))
    , _neNoiseDev(td::real4, gui::LineEdit::Messages::DoNotSend, false, tr("Measurement noise standard deviation"), 4)
    , _lblSectionProgress(tr("Progress & status"), gui::Font::ID::SystemBold)
    , _lblProgress(tr("Progress:"))
    , _lblStatus(tr("Status:"))
    , _lblSectionInfo(tr("Input file preview"), gui::Font::ID::SystemBold)
    , _btnSelectInFn("...")
    , _btnSelectOutFn("...")
    , _btnInfo(tr("Info"))
    , _btnConvert(tr("Convert"))
    , _hlButtons(3)
    , _gl(13, 3)
    , _asyncUpdateProgress(std::bind(&ViewConv::updateProgressUI, this))
    , _asyncOnConversionDone(std::bind(&ViewConv::onConversionDone, this))
    {
        assert(_pIPlugin);
        _editModelName = "Three-phase state estimation in rectangular coordinates";
        _chkNoise.setChecked(false);
        _neNoiseDev.setValue(0.01f);
        _editProgress = "0%";
        _editProgress.setAsReadOnly();
        _editStatus.setAsReadOnly();
        _teInfo.setAsReadOnly();

        _teInfo.setText(
            "Expected input: a Matpower (.m) case file with a three-phase bus block. Two layouts\n"
            "are accepted:\n\n"
            "1) Custom 'bus3ph' (7 columns):\n"
            "mpc.bus3ph = [\n"
            "    busId  Vm_A  Va_A  Vm_B  Va_B  Vm_C  Va_C;\n"
            "    ...\n"
            "];\n\n"
            "2) Standard Matpower 'bus3p' (9 columns, e.g. t_case3p_a.m):\n"
            "mpc.bus3p = [\n"
            "    busid  type  basekV  Vm1  Vm2  Vm3  Va1  Va2  Va3;\n"
            "    ...\n"
            "];\n\n"
            "Vm_x = voltage magnitude of phase x [p.u.], Va_x = voltage angle of phase x [degrees].\n"
            "Standard Matpower blocks (mpc.bus, mpc.branch, mpc.gen, mpc.baseMVA, ...) may also be\n"
            "present in the file and are ignored by this converter.\n\n"
            "The converter computes, for every node and every phase (A, B, C):\n"
            "    Vr = Vm * cos(Va)   (real / rectangular component)\n"
            "    Vm_rect = Vm * sin(Va)   (imaginary / rectangular component)\n\n"
            "Add measurement noise (optional): when ticked, each voltage component gets\n"
            "several independent noisy measurements (Gauss, std.dev = Noise std.dev), so the\n"
            "WLS solver performs real state estimation - it filters the noise and reports the\n"
            "estimation-error statistics (minErr/maxErr/avgErr/devErr) in the solution.\n"
            "Best used on small/medium cases; leave unticked for an exact conversion.\n");

        gui::GridComposer gc(_gl);
        gc.appendRow(_lblSectionIO, 0);
        gc.appendRow(_lblFnIn) << _editFnIn << _btnSelectInFn;
        gc.appendRow(_lblFnOut) << _editFnOut << _btnSelectOutFn;
        gc.appendRow(_lblSectionOptions, 0);
        gc.appendRow(_lblModelName); gc.appendCol(_editModelName, 0);
        gc.appendRow(_chkNoise, 0);
        gc.appendRow(_lblNoiseDev) << _neNoiseDev;
        gc.appendRow(_lblSectionProgress, 0);
        gc.appendRow(_lblProgress) << _progIndicator << _editProgress;
        gc.appendRow(_lblStatus); gc.appendCol(_editStatus, 0);
        gc.appendRow(_lblSectionInfo, 0);
        gc.appendRow(_teInfo, 0);
        _hlButtons.appendSpacer() << _btnInfo << _btnConvert;
        gc.appendRow(_hlButtons, 0);

        setLayout(&_gl);
        handleUserActions();
    }

    ~ViewConv()
    {
        _closing = true;
        if (_convThread.joinable())
            _convThread.join();
    }

    td::String getOutFileName() const
    {
        return _editFnOut.getText();
    }
};
