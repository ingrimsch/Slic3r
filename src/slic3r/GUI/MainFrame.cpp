#include "MainFrame.hpp"

#include <wx/panel.h>
#include <wx/notebook.h>
#include <wx/icon.h>
#include <wx/sizer.h>
#include <wx/menu.h>
#include <wx/progdlg.h>
#include <wx/tooltip.h>
#include <wx/glcanvas.h>
#include <wx/debug.h>

#include "libslic3r/Print.hpp"
#include "libslic3r/Polygon.hpp"

#include "Tab.hpp"
#include "PresetBundle.hpp"
#include "ProgressStatusBar.hpp"
#include "3DScene.hpp"
#include "AppConfig.hpp"
#include "wxExtensions.hpp"
#include "I18N.hpp"

#include <fstream>
#include "GUI_App.hpp"

namespace Slic3r {
namespace GUI {

MainFrame::MainFrame(const bool no_plater, const bool loaded) :
wxFrame(NULL, wxID_ANY, SLIC3R_BUILD, wxDefaultPosition, wxDefaultSize, wxDEFAULT_FRAME_STYLE),
        m_no_plater(no_plater),
        m_loaded(loaded)
{
    // Load the icon either from the exe, or from the ico file.
#if _WIN32
    {
        TCHAR szExeFileName[MAX_PATH];
        GetModuleFileName(nullptr, szExeFileName, MAX_PATH);
        SetIcon(wxIcon(szExeFileName, wxBITMAP_TYPE_ICO));
    }
#else
    SetIcon(wxIcon(Slic3r::var("Slic3r_128px.png"), wxBITMAP_TYPE_PNG));
#endif // _WIN32

	// initialize status bar
	m_statusbar = new ProgressStatusBar(this);
	m_statusbar->embed(this);
	m_statusbar->set_status_text(_(L("Version ")) +
		SLIC3R_VERSION +
		_(L(" - Remember to check for updates at http://github.com/prusa3d/slic3r/releases")));

    // initialize tabpanel and menubar
    init_tabpanel();
    init_menubar();

    // set default tooltip timer in msec
    // SetAutoPop supposedly accepts long integers but some bug doesn't allow for larger values
    // (SetAutoPop is not available on GTK.)
    wxToolTip::SetAutoPop(32767);

    m_loaded = true;

    // initialize layout
    auto sizer = new wxBoxSizer(wxVERTICAL);
    if (m_tabpanel)
        sizer->Add(m_tabpanel, 1, wxEXPAND);
    sizer->SetSizeHints(this);
    SetSizer(sizer);
    Fit();
    SetMinSize(wxSize(760, 490));
    SetSize(GetMinSize());
    Layout();

    // declare events
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
        if (event.CanVeto() && !wxGetApp().check_unsaved_changes()) {
            event.Veto();
            return;
        }
        // save window size
        wxGetApp().window_pos_save(this, "mainframe");
        // Save the slic3r.ini.Usually the ini file is saved from "on idle" callback,
        // but in rare cases it may not have been called yet.
        wxGetApp().app_config->save();
//         if (m_plater)
//             m_plater->print = undef;
        _3DScene::remove_all_canvases();
//         Slic3r::GUI::deregister_on_request_update_callback();
        // propagate event
        event.Skip();
    });

    // NB: Restoring the window position is done in a two-phase manner here,
    // first the saved position is restored as-is and validation is done after the window is shown
    // and initial round of events is complete, because on some platforms that is the only way
    // to get an accurate window position & size.
    wxGetApp().window_pos_restore(this, "mainframe");
    Bind(wxEVT_SHOW, [this](wxShowEvent&) {
        CallAfter([this]() {
            wxGetApp().window_pos_sanitize(this);
        });
    });

    update_ui_from_settings();
}

void MainFrame::init_tabpanel()
{
    m_tabpanel = new wxNotebook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxNB_TOP | wxTAB_TRAVERSAL);

    m_tabpanel->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, [this](wxEvent&) {
        auto panel = m_tabpanel->GetCurrentPage();

        if (panel == nullptr)
            return;

        auto& tabs_list = wxGetApp().tabs_list;
        if (find(tabs_list.begin(), tabs_list.end(), panel) != tabs_list.end()) {
            // On GTK, the wxEVT_NOTEBOOK_PAGE_CHANGED event is triggered
            // before the MainFrame is fully set up.
            static_cast<Tab*>(panel)->OnActivate();
        }
    });

    if (!m_no_plater) {
        m_plater = new Slic3r::GUI::Plater(m_tabpanel, this);
        wxGetApp().plater_ = m_plater;
        m_tabpanel->AddPage(m_plater, _(L("Plater")));
    }

    // The following event is emited by Tab implementation on config value change.
    Bind(EVT_TAB_VALUE_CHANGED, &MainFrame::on_value_changed, this);

    // The following event is emited by Tab on preset selection,
    // or when the preset's "modified" status changes.
    Bind(EVT_TAB_PRESETS_CHANGED, &MainFrame::on_presets_changed, this);

    create_preset_tabs();

    if (m_plater) {
        // load initial config
        auto full_config = wxGetApp().preset_bundle->full_config();
        m_plater->on_config_change(full_config);

        // Show a correct number of filament fields.
        // nozzle_diameter is undefined when SLA printer is selected
        if (full_config.has("nozzle_diameter")) {
            m_plater->on_extruders_change(full_config.option<ConfigOptionFloats>("nozzle_diameter")->values.size());
        }
    }
}

void MainFrame::create_preset_tabs()
{
    wxGetApp().update_label_colours_from_appconfig();
    add_created_tab(new TabPrint(m_tabpanel));
    add_created_tab(new TabFilament(m_tabpanel));
    add_created_tab(new TabSLAPrint(m_tabpanel));
    add_created_tab(new TabSLAMaterial(m_tabpanel));
    add_created_tab(new TabPrinter(m_tabpanel));
}

void MainFrame::add_created_tab(Tab* panel)
{
    panel->create_preset_tab();

    const auto printer_tech = wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology();

    if (panel->supports_printer_technology(printer_tech))
        m_tabpanel->AddPage(panel, panel->title());
}

bool MainFrame::can_save() const
{
    return (m_plater != nullptr) ? !m_plater->model().objects.empty() : false;
}

bool MainFrame::can_export_model() const
{
    return (m_plater != nullptr) ? !m_plater->model().objects.empty() : false;
}

bool MainFrame::can_export_gcode() const
{
    if (m_plater == nullptr)
        return false;

    if (m_plater->model().objects.empty())
        return false;

    if (m_plater->is_export_gcode_scheduled())
        return false;

    // TODO:: add other filters

    return true;
}

bool MainFrame::can_change_view() const
{
    int page_id = m_tabpanel->GetSelection();
    return (page_id != wxNOT_FOUND) ? m_tabpanel->GetPageText((size_t)page_id).Lower() == "plater" : false;
}

bool MainFrame::can_select() const
{
    return (m_plater != nullptr) ? !m_plater->model().objects.empty() : false;
}

bool MainFrame::can_delete() const
{
    return (m_plater != nullptr) ? !m_plater->is_selection_empty() : false;
}

bool MainFrame::can_delete_all() const
{
    return (m_plater != nullptr) ? !m_plater->model().objects.empty() : false;
}

void MainFrame::init_menubar()
{
    // File menu
    wxMenu* fileMenu = new wxMenu;
    {
        wxMenuItem* item_open = append_menu_item(fileMenu, wxID_ANY, _(L("Open…\tCtrl+O")), _(L("Open a project file")),
            [this](wxCommandEvent&) { if (m_plater) m_plater->load_project(); }, "brick_add.png");
        wxMenuItem* item_save = append_menu_item(fileMenu, wxID_ANY, _(L("Save\tCtrl+S")), _(L("Save current project file")),
            [this](wxCommandEvent&) { if (m_plater) m_plater->export_3mf(m_plater->get_project_filename().wx_str()); }, "disk.png");
        wxMenuItem* item_save_as = append_menu_item(fileMenu, wxID_ANY, _(L("Save as…\tCtrl+Alt+S")), _(L("Save current project file as")),
            [this](wxCommandEvent&) { if (m_plater) m_plater->export_3mf(); }, "disk.png");

        fileMenu->AppendSeparator();

        wxMenu* import_menu = new wxMenu();
        wxMenuItem* item_import_model = append_menu_item(import_menu, wxID_ANY, _(L("Import STL/OBJ/AMF/3MF…\tCtrl+I")), _(L("Load a model")),
            [this](wxCommandEvent&) { if (m_plater) m_plater->add_model(); }, "brick_add.png");
        import_menu->AppendSeparator();
        append_menu_item(import_menu, wxID_ANY, _(L("Import Config…\tCtrl+L")), _(L("Load exported configuration file")),
            [this](wxCommandEvent&) { load_config_file(); }, "plugin_add.png");
        append_menu_item(import_menu, wxID_ANY, _(L("Import Config from project…\tCtrl+Alt+L")), _(L("Load configuration from project file")),
            [this](wxCommandEvent&) { if (m_plater) m_plater->extract_config_from_project(); }, "plugin_add.png");
        import_menu->AppendSeparator();
        append_menu_item(import_menu, wxID_ANY, _(L("Import Config Bundle…")), _(L("Load presets from a bundle")),
            [this](wxCommandEvent&) { load_configbundle(); }, "lorry_add.png");
        append_submenu(fileMenu, import_menu, wxID_ANY, _(L("Import")), _(L("")));

        wxMenu* export_menu = new wxMenu();
        wxMenuItem* item_export_gcode = append_menu_item(export_menu, wxID_ANY, _(L("Export G-code…\tCtrl+G")), _(L("Export current plate as G-code")),
            [this](wxCommandEvent&) { if (m_plater) m_plater->export_gcode(); }, "cog_go.png");
        export_menu->AppendSeparator();
        wxMenuItem* item_export_stl = append_menu_item(export_menu, wxID_ANY, _(L("Export plate as STL…")), _(L("Export current plate as STL")),
            [this](wxCommandEvent&) { if (m_plater) m_plater->export_stl(); }, "brick_go.png");
        wxMenuItem* item_export_amf = append_menu_item(export_menu, wxID_ANY, _(L("Export plate as AMF…")), _(L("Export current plate as AMF")),
            [this](wxCommandEvent&) { if (m_plater) m_plater->export_amf(); }, "brick_go.png");
        export_menu->AppendSeparator();
        append_menu_item(export_menu, wxID_ANY, _(L("Export Config…\tCtrl+E")), _(L("Export current configuration to file")),
            [this](wxCommandEvent&) { export_config(); }, "plugin_go.png");
        append_menu_item(export_menu, wxID_ANY, _(L("Export Config Bundle…")), _(L("Export all presets to file")),
            [this](wxCommandEvent&) { export_configbundle(); }, "lorry_go.png");
        append_submenu(fileMenu, export_menu, wxID_ANY, _(L("Export")), _(L("")));

        fileMenu->AppendSeparator();

#if 0
        m_menu_item_repeat = nullptr;
        append_menu_item(fileMenu, wxID_ANY, _(L("Quick Slice…\tCtrl+U")), _(L("Slice a file into a G-code")),
            [this](wxCommandEvent&) {
                wxTheApp->CallAfter([this]() {
                    quick_slice();
                    m_menu_item_repeat->Enable(is_last_input_file());
                }); }, "cog_go.png");
        append_menu_item(fileMenu, wxID_ANY, _(L("Quick Slice and Save As…\tCtrl+Alt+U")), _(L("Slice a file into a G-code, save as")),
            [this](wxCommandEvent&) {
            wxTheApp->CallAfter([this]() {
                    quick_slice(qsSaveAs);
                    m_menu_item_repeat->Enable(is_last_input_file());
                }); }, "cog_go.png");
        m_menu_item_repeat = append_menu_item(fileMenu, wxID_ANY, _(L("Repeat Last Quick Slice\tCtrl+Shift+U")), _(L("Repeat last quick slice")),
            [this](wxCommandEvent&) {
            wxTheApp->CallAfter([this]() {
                quick_slice(qsReslice);
            }); }, "cog_go.png");
        m_menu_item_repeat->Enable(false);
        fileMenu->AppendSeparator();
#endif
        m_menu_item_reslice_now = append_menu_item(fileMenu, wxID_ANY, _(L("(Re)Slice Now\tCtrl+R")), _(L("Start new slicing process")),
            [this](wxCommandEvent&) { reslice_now(); }, "shape_handles.png");
        fileMenu->AppendSeparator();
        append_menu_item(fileMenu, wxID_ANY, _(L("Repair STL file…")), _(L("Automatically repair an STL file")),
            [this](wxCommandEvent&) { repair_stl(); }, "wrench.png");
        fileMenu->AppendSeparator();
        append_menu_item(fileMenu, wxID_EXIT, _(L("Quit")), _(L("Quit Slic3r")),
            [this](wxCommandEvent&) { Close(false); });

        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable(m_plater != nullptr); }, item_open->GetId());
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable((m_plater != nullptr) && can_save()); }, item_save->GetId());
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable((m_plater != nullptr) && can_save()); }, item_save_as->GetId());
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable(m_plater != nullptr); }, item_import_model->GetId());
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable((m_plater != nullptr) && can_export_gcode()); }, item_export_gcode->GetId());
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable((m_plater != nullptr) && can_export_model()); }, item_export_stl->GetId());
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable((m_plater != nullptr) && can_export_model()); }, item_export_amf->GetId());
    }

    // Edit menu
    wxMenu* editMenu = nullptr;
    if (m_plater != nullptr)
    {
        editMenu = new wxMenu();
        wxMenuItem* item_select_all = append_menu_item(editMenu, wxID_ANY, L("Select all\tCtrl+A"), L("Selects all objects"),
            [this](wxCommandEvent&) { m_plater->select_all(); }, "");
        editMenu->AppendSeparator();
        wxMenuItem* item_delete_sel = append_menu_item(editMenu, wxID_ANY, L("Delete selected\tDel"), L("Deletes the current selection"),
            [this](wxCommandEvent&) { m_plater->remove_selected(); }, "");
        wxMenuItem* item_delete_all = append_menu_item(editMenu, wxID_ANY, L("Delete all\tCtrl+Del"), L("Deletes all objects"),
            [this](wxCommandEvent&) { m_plater->reset(); }, "");

        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable(can_select()); }, item_select_all->GetId());
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable(can_delete()); }, item_delete_sel->GetId());
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable(can_delete_all()); }, item_delete_all->GetId());
    }

    // Window menu
    auto windowMenu = new wxMenu();
    {
        size_t tab_offset = 0;
        if (m_plater) {
#if ENABLE_REMOVE_TABS_FROM_PLATER
            append_menu_item(windowMenu, wxID_ANY, L("Plater Tab\tCtrl+1"), L("Show the plater"),
                [this](wxCommandEvent&) { select_tab(0); }, "application_view_tile.png");
#else
            append_menu_item(windowMenu, wxID_ANY, L("Select Plater Tab\tCtrl+1"), L("Show the plater"),
                [this](wxCommandEvent&) { select_tab(0); }, "application_view_tile.png");
#endif // ENABLE_REMOVE_TABS_FROM_PLATER
            tab_offset += 1;
        }
        if (tab_offset > 0) {
            windowMenu->AppendSeparator();
        }
#if ENABLE_REMOVE_TABS_FROM_PLATER
        append_menu_item(windowMenu, wxID_ANY, L("Print Settings Tab\tCtrl+2"), L("Show the print settings"),
            [this, tab_offset](wxCommandEvent&) { select_tab(tab_offset + 0); }, "cog.png");
        append_menu_item(windowMenu, wxID_ANY, L("Filament Settings Tab\tCtrl+3"), L("Show the filament settings"),
            [this, tab_offset](wxCommandEvent&) { select_tab(tab_offset + 1); }, "spool.png");
        append_menu_item(windowMenu, wxID_ANY, L("Printer Settings Tab\tCtrl+4"), L("Show the printer settings"),
            [this, tab_offset](wxCommandEvent&) { select_tab(tab_offset + 2); }, "printer_empty.png");
        if (m_plater) {
            windowMenu->AppendSeparator();
            wxMenuItem* item_3d = append_menu_item(windowMenu, wxID_ANY, L("3D\tCtrl+5"), L("Show the 3D editing view"),
            [this](wxCommandEvent&) { m_plater->select_view_3D("3D"); }, "");
            wxMenuItem* item_preview = append_menu_item(windowMenu, wxID_ANY, L("Preview\tCtrl+6"), L("Show the 3D slices preview"),
            [this](wxCommandEvent&) { m_plater->select_view_3D("Preview"); }, "");

            Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable(can_change_view()); }, item_3d->GetId());
            Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable(can_change_view()); }, item_preview->GetId());
        }
#else
        append_menu_item(windowMenu, wxID_ANY, L("Select Print Settings Tab\tCtrl+2"), L("Show the print settings"),
            [this, tab_offset](wxCommandEvent&) { select_tab(tab_offset + 0); }, "cog.png");
        append_menu_item(windowMenu, wxID_ANY, L("Select Filament Settings Tab\tCtrl+3"), L("Show the filament settings"),
            [this, tab_offset](wxCommandEvent&) { select_tab(tab_offset + 1); }, "spool.png");
        append_menu_item(windowMenu, wxID_ANY, L("Select Printer Settings Tab\tCtrl+4"), L("Show the printer settings"),
            [this, tab_offset](wxCommandEvent&) { select_tab(tab_offset + 2); }, "printer_empty.png");
#endif // ENABLE_REMOVE_TABS_FROM_PLATER
    }

    // View menu
    wxMenu* viewMenu = nullptr;
    if (m_plater) {
        viewMenu = new wxMenu();
        // \xA0 is a non-breaing space. It is entered here to spoil the automatic accelerators,
        // as the simple numeric accelerators spoil all numeric data entry.
        // The camera control accelerators are captured by GLCanvas3D::on_char().
        wxMenuItem* item_iso = append_menu_item(viewMenu, wxID_ANY, _(L("Iso")) + "\t\xA0" + "0", _(L("Iso View")), [this](wxCommandEvent&) { select_view("iso"); });
        viewMenu->AppendSeparator();
        wxMenuItem* item_top = append_menu_item(viewMenu, wxID_ANY, _(L("Top")) + "\t\xA0" + "1", _(L("Top View")), [this](wxCommandEvent&) { select_view("top"); });
        wxMenuItem* item_bottom = append_menu_item(viewMenu, wxID_ANY, _(L("Bottom")) + "\t\xA0" + "2", _(L("Bottom View")), [this](wxCommandEvent&) { select_view("bottom"); });
        wxMenuItem* item_front = append_menu_item(viewMenu, wxID_ANY, _(L("Front")) + "\t\xA0" + "3", _(L("Front View")), [this](wxCommandEvent&) { select_view("front"); });
        wxMenuItem* item_rear = append_menu_item(viewMenu, wxID_ANY, _(L("Rear")) + "\t\xA0" + "4", _(L("Rear View")), [this](wxCommandEvent&) { select_view("rear"); });
        wxMenuItem* item_left = append_menu_item(viewMenu, wxID_ANY, _(L("Left")) + "\t\xA0" + "5", _(L("Left View")), [this](wxCommandEvent&) { select_view("left"); });
        wxMenuItem* item_right = append_menu_item(viewMenu, wxID_ANY, _(L("Right")) + "\t\xA0" + "6", _(L("Right View")), [this](wxCommandEvent&) { select_view("right"); });

        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable(can_change_view()); }, item_iso->GetId());
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable(can_change_view()); }, item_top->GetId());
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable(can_change_view()); }, item_bottom->GetId());
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable(can_change_view()); }, item_front->GetId());
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable(can_change_view()); }, item_rear->GetId());
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable(can_change_view()); }, item_left->GetId());
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable(can_change_view()); }, item_right->GetId());
    }

    // Help menu
    auto helpMenu = new wxMenu();
    {
        append_menu_item(helpMenu, wxID_ANY, _(L("Prusa 3D Drivers")), _(L("Open the Prusa3D drivers download page in your browser")), 
            [this](wxCommandEvent&) { wxLaunchDefaultBrowser("http://www.prusa3d.com/drivers/"); });
        append_menu_item(helpMenu, wxID_ANY, _(L("Prusa Edition Releases")), _(L("Open the Prusa Edition releases page in your browser")), 
            [this](wxCommandEvent&) { wxLaunchDefaultBrowser("http://github.com/prusa3d/slic3r/releases"); });
//#        my $versioncheck = $self->_append_menu_item($helpMenu, "Check for &Updates...", "Check for new Slic3r versions", sub{
//#            wxTheApp->check_version(1);
//#        });
//#        $versioncheck->Enable(wxTheApp->have_version_check);
        append_menu_item(helpMenu, wxID_ANY, _(L("Slic3r Website")), _(L("Open the Slic3r website in your browser")),
            [this](wxCommandEvent&) { wxLaunchDefaultBrowser("http://slic3r.org/"); });
        append_menu_item(helpMenu, wxID_ANY, _(L("Slic3r Manual")), _(L("Open the Slic3r manual in your browser")),
            [this](wxCommandEvent&) { wxLaunchDefaultBrowser("http://manual.slic3r.org/"); });
        helpMenu->AppendSeparator();
        append_menu_item(helpMenu, wxID_ANY, _(L("System Info")), _(L("Show system information")), 
            [this](wxCommandEvent&) { wxGetApp().system_info(); });
        append_menu_item(helpMenu, wxID_ANY, _(L("Show Configuration Folder")), _(L("Show user configuration folder (datadir)")),
            [this](wxCommandEvent&) { Slic3r::GUI::desktop_open_datadir_folder(); });
        append_menu_item(helpMenu, wxID_ANY, _(L("Report an Issue")), _(L("Report an issue on the Slic3r Prusa Edition")), 
            [this](wxCommandEvent&) { wxLaunchDefaultBrowser("http://github.com/prusa3d/slic3r/issues/new"); });
        append_menu_item(helpMenu, wxID_ANY, _(L("About Slic3r")), _(L("Show about dialog")),
            [this](wxCommandEvent&) { Slic3r::GUI::about(); });
    }

    // menubar
    // assign menubar to frame after appending items, otherwise special items
    // will not be handled correctly
    {
        auto menubar = new wxMenuBar();
        menubar->Append(fileMenu, L("&File"));
        if (editMenu) menubar->Append(editMenu, L("&Edit"));
        menubar->Append(windowMenu, L("&Window"));
        if (viewMenu) menubar->Append(viewMenu, L("&View"));
        // Add additional menus from C++
        wxGetApp().add_config_menu(menubar);
        menubar->Append(helpMenu, L("&Help"));
        SetMenuBar(menubar);
    }
}

// To perform the "Quck Slice", "Quick Slice and Save As", "Repeat last Quick Slice" and "Slice to SVG".
void MainFrame::quick_slice(const int qs)
{
//     my $progress_dialog;
    wxString input_file;
//  eval
//     {
    // validate configuration
    auto config = wxGetApp().preset_bundle->full_config();
    config.validate();

    // select input file
    if (!(qs & qsReslice)) {
        auto dlg = new wxFileDialog(this, _(L("Choose a file to slice (STL/OBJ/AMF/3MF/PRUSA):")),
            wxGetApp().app_config->get_last_dir(), "",
            file_wildcards(FT_MODEL), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg->ShowModal() != wxID_OK) {
            dlg->Destroy();
            return;
        }
        input_file = dlg->GetPath();
        dlg->Destroy();
        if (!(qs & qsExportSVG))
            m_qs_last_input_file = input_file;
    }
    else {
        if (m_qs_last_input_file.IsEmpty()) {
            auto dlg = new wxMessageDialog(this, _(L("No previously sliced file.")),
                _(L("Error")), wxICON_ERROR | wxOK);
            dlg->ShowModal();
            return;
        }
        if (std::ifstream(m_qs_last_input_file.char_str())) {
            auto dlg = new wxMessageDialog(this, _(L("Previously sliced file ("))+m_qs_last_input_file+_(L(") not found.")),
                _(L("File Not Found")), wxICON_ERROR | wxOK);
            dlg->ShowModal();
            return;
        }
        input_file = m_qs_last_input_file;
    }
    auto input_file_basename = get_base_name(input_file);
    wxGetApp().app_config->update_skein_dir(get_dir_name(input_file));

    auto bed_shape = Slic3r::Polygon::new_scale(config.option<ConfigOptionPoints>("bed_shape")->values);
//     auto print_center = Slic3r::Pointf->new_unscale(bed_shape.bounding_box().center());
// 
//     auto sprint = new Slic3r::Print::Simple(
//         print_center = > print_center,
//         status_cb = > [](int percent, const wxString& msg) {
//         m_progress_dialog->Update(percent, msg+"…");
//     });

    // keep model around
    auto model = Slic3r::Model::read_from_file(input_file.ToStdString());

//     sprint->apply_config(config);
//     sprint->set_model(model);

    // Copy the names of active presets into the placeholder parser.
//     wxGetApp().preset_bundle->export_selections(sprint->placeholder_parser);

    // select output file
    wxString output_file;
    if (qs & qsReslice) {
        if (!m_qs_last_output_file.IsEmpty())
            output_file = m_qs_last_output_file;
    } 
    else if (qs & qsSaveAs) {
        // The following line may die if the output_filename_format template substitution fails.
//         output_file = sprint->output_filepath;
//         if (export_svg)
//         output_file = ~s / \.[gG][cC][oO][dD][eE]$ / .svg /;
        auto dlg = new wxFileDialog(this, _(L("Save ")) + (qs & qsExportSVG ? _(L("SVG")) : _(L("G-code"))) + _(L(" file as:")),
            wxGetApp().app_config->get_last_output_dir(get_dir_name(output_file)), get_base_name(input_file), 
            qs & qsExportSVG ? file_wildcards(FT_SVG) : file_wildcards(FT_GCODE),
            wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg->ShowModal() != wxID_OK) {
            dlg->Destroy();
            return;
        }
        output_file = dlg->GetPath();
        dlg->Destroy();
        if (!(qs & qsExportSVG))
            m_qs_last_output_file = output_file;
        wxGetApp().app_config->update_last_output_dir(get_dir_name(output_file));
    } 
    else if (qs & qsExportPNG) {
//         output_file = sprint->output_filepath;
//         output_file = ~s / \.[gG][cC][oO][dD][eE]$ / .zip / ;
        auto dlg = new wxFileDialog(this, _(L("Save zip file as:")),
            wxGetApp().app_config->get_last_output_dir(get_dir_name(output_file)),
            get_base_name(output_file), "*.zip", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg->ShowModal() != wxID_OK) {
            dlg->Destroy();
            return;
        }
        output_file = dlg->GetPath();
        dlg->Destroy();
    }

    // show processbar dialog
    m_progress_dialog = new wxProgressDialog(_(L("Slicing…")), _(L("Processing ")) + input_file_basename + "…",
        100, this, 4);
    m_progress_dialog->Pulse();
    {
//         my @warnings = ();
//         local $SIG{ __WARN__ } = sub{ push @warnings, $_[0] };

//         sprint->output_file(output_file);
//         if (export_svg) {
//             sprint->export_svg();
//         }
//         else if(export_png) {
//             sprint->export_png();
//         }
//         else {
//             sprint->export_gcode();
//         }
//         sprint->status_cb(undef);
//         Slic3r::GUI::warning_catcher($self)->($_) for @warnings;
    }
    m_progress_dialog->Destroy();
    m_progress_dialog = nullptr;

    auto message = input_file_basename + _(L(" was successfully sliced."));
//     wxTheApp->notify(message);
    wxMessageDialog(this, message, _(L("Slicing Done!")), wxOK | wxICON_INFORMATION).ShowModal();
//     };
//     Slic3r::GUI::catch_error(this, []() { if (m_progress_dialog) m_progress_dialog->Destroy(); });
}

void MainFrame::reslice_now()
{
    if (m_plater)
        m_plater->reslice();
}

void MainFrame::repair_stl()
{
    wxString input_file;
    {
        auto dlg = new wxFileDialog(this, _(L("Select the STL file to repair:")),
            wxGetApp().app_config->get_last_dir(), "",
            file_wildcards(FT_STL), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg->ShowModal() != wxID_OK) {
            dlg->Destroy();
            return;
        }
        input_file = dlg->GetPath();
        dlg->Destroy();
    }

    auto output_file = input_file;
    {
//         output_file = ~s / \.[sS][tT][lL]$ / _fixed.obj / ;
        auto dlg = new wxFileDialog( this, L("Save OBJ file (less prone to coordinate errors than STL) as:"), 
                                        get_dir_name(output_file), get_base_name(output_file), 
                                        file_wildcards(FT_OBJ), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg->ShowModal() != wxID_OK) {
            dlg->Destroy();
            return /*undef*/;
        }
        output_file = dlg->GetPath();
        dlg->Destroy();
    }

    auto tmesh = new Slic3r::TriangleMesh();
    tmesh->ReadSTLFile(input_file.char_str());
    tmesh->repair();
    tmesh->WriteOBJFile(output_file.char_str());
    Slic3r::GUI::show_info(this, L("Your file was repaired."), L("Repair"));
}

void MainFrame::export_config()
{
    // Generate a cummulative configuration for the selected print, filaments and printer.
    auto config = wxGetApp().preset_bundle->full_config();
    // Validate the cummulative configuration.
    auto valid = config.validate();
    if (!valid.empty()) {
//         Slic3r::GUI::catch_error(this);
        return;
    }
    // Ask user for the file name for the config file.
    auto dlg = new wxFileDialog(this, _(L("Save configuration as:")),
        !m_last_config.IsEmpty() ? get_dir_name(m_last_config) : wxGetApp().app_config->get_last_dir(),
        !m_last_config.IsEmpty() ? get_base_name(m_last_config) : "config.ini",
        file_wildcards(FT_INI), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    wxString file;
    if (dlg->ShowModal() == wxID_OK)
        file = dlg->GetPath();
    dlg->Destroy();
    if (!file.IsEmpty()) {
        wxGetApp().app_config->update_config_dir(get_dir_name(file));
        m_last_config = file;
        config.save(file.ToStdString());
    }
}

// Load a config file containing a Print, Filament & Printer preset.
void MainFrame::load_config_file(wxString file/* = wxEmptyString*/)
{
    if (file.IsEmpty()) {
        if (!wxGetApp().check_unsaved_changes())
            return;
        auto dlg = new wxFileDialog(this, _(L("Select configuration to load:")),
            !m_last_config.IsEmpty() ? get_dir_name(m_last_config) : wxGetApp().app_config->get_last_dir(),
            "config.ini", "INI files (*.ini, *.gcode)|*.ini;*.INI;*.gcode;*.g", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg->ShowModal() != wxID_OK)
            return;
        file = dlg->GetPath();
        dlg->Destroy();
    }
    try {
        wxGetApp().preset_bundle->load_config_file(file.ToStdString()); 
    } catch (const std::exception &ex) {
        show_error(this, ex.what());
        return;
    }
	wxGetApp().load_current_presets();
    wxGetApp().app_config->update_config_dir(get_dir_name(file));
    m_last_config = file;
}

void MainFrame::export_configbundle()
{
    if (!wxGetApp().check_unsaved_changes())
        return;
    // validate current configuration in case it's dirty
    auto err = wxGetApp().preset_bundle->full_config().validate();
    if (! err.empty()) {
        show_error(this, err);
        return;
    }
    // Ask user for a file name.
    auto dlg = new wxFileDialog(this, _(L("Save presets bundle as:")),
        !m_last_config.IsEmpty() ? get_dir_name(m_last_config) : wxGetApp().app_config->get_last_dir(),
        "Slic3r_config_bundle.ini",
        file_wildcards(FT_INI), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    wxString file;
    if (dlg->ShowModal() == wxID_OK)
        file = dlg->GetPath();
    dlg->Destroy();
    if (!file.IsEmpty()) {
        // Export the config bundle.
        wxGetApp().app_config->update_config_dir(get_dir_name(file));
        try {
            wxGetApp().preset_bundle->export_configbundle(file.ToStdString()); 
        } catch (const std::exception &ex) {
			show_error(this, ex.what());
        }
    }
}

// Loading a config bundle with an external file name used to be used
// to auto - install a config bundle on a fresh user account,
// but that behavior was not documented and likely buggy.
void MainFrame::load_configbundle(wxString file/* = wxEmptyString, const bool reset_user_profile*/)
{
    if (!wxGetApp().check_unsaved_changes())
        return;
    if (file.IsEmpty()) {
        auto dlg = new wxFileDialog(this, _(L("Select configuration to load:")),
            !m_last_config.IsEmpty() ? get_dir_name(m_last_config) : wxGetApp().app_config->get_last_dir(),
            "config.ini", file_wildcards(FT_INI), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg->ShowModal() != wxID_OK)
            return;
        file = dlg->GetPath();
        dlg->Destroy();
    }

    wxGetApp().app_config->update_config_dir(get_dir_name(file));

    auto presets_imported = 0;
    try {
        presets_imported = wxGetApp().preset_bundle->load_configbundle(file.ToStdString());
    } catch (const std::exception &ex) {
        show_error(this, ex.what());
        return;
    }

    // Load the currently selected preset into the GUI, update the preset selection box.
	wxGetApp().load_current_presets();

    const auto message = wxString::Format(_(L("%d presets successfully imported.")), presets_imported);
    Slic3r::GUI::show_info(this, message, "Info");
}

// Load a provied DynamicConfig into the Print / Filament / Printer tabs, thus modifying the active preset.
// Also update the platter with the new presets.
void MainFrame::load_config(const DynamicPrintConfig& config)
{
    for (auto tab : wxGetApp().tabs_list)
        tab->load_config(config);
    if (m_plater) 
        m_plater->on_config_change(config);
}

void MainFrame::select_tab(size_t tab) const
{
    m_tabpanel->SetSelection(tab);
}

// Set a camera direction, zoom to all objects.
void MainFrame::select_view(const std::string& direction)
{
     if (m_plater)
         m_plater->select_view(direction);
}

void MainFrame::on_presets_changed(SimpleEvent &event)
{
    auto *tab = dynamic_cast<Tab*>(event.GetEventObject());
    wxASSERT(tab != nullptr);
    if (tab == nullptr) {
        return;
    }

    // Update preset combo boxes(Print settings, Filament, Material, Printer) from their respective tabs.
    auto presets = tab->get_presets();
    if (m_plater != nullptr && presets != nullptr) {

        // FIXME: The preset type really should be a property of Tab instead
        Slic3r::Preset::Type preset_type = tab->type();
        if (preset_type == Slic3r::Preset::TYPE_INVALID) {
            wxASSERT(false);
            return;
        }

        m_plater->on_config_change(*tab->get_config());
        m_plater->sidebar().update_presets(preset_type);
    }
}

void MainFrame::on_value_changed(wxCommandEvent& event)
{
    auto *tab = dynamic_cast<Tab*>(event.GetEventObject());
    wxASSERT(tab != nullptr);
    if (tab == nullptr)
        return;

    auto opt_key = event.GetString();
    if (m_plater) {
        m_plater->on_config_change(*tab->get_config()); // propagate config change events to the plater
        if (opt_key == "extruders_count") {
            auto value = event.GetInt();
            m_plater->on_extruders_change(value);
        }
    }
    // Don't save while loading for the first time.
    if (m_loaded) {
        AppConfig &cfg = *wxGetApp().app_config;
        if (cfg.get("autosave") == "1")
            cfg.save();
    }
}

// Called after the Preferences dialog is closed and the program settings are saved.
// Update the UI based on the current preferences.
void MainFrame::update_ui_from_settings()
{
    bool bp_on = wxGetApp().app_config->get("background_processing") == "1";
    m_menu_item_reslice_now->Enable(bp_on);
    m_plater->sidebar().show_reslice(!bp_on);
    m_plater->sidebar().Layout();
    if (m_plater)
        m_plater->update_ui_from_settings();
    for (auto tab: wxGetApp().tabs_list)
        tab->update_ui_from_settings();
}

std::string MainFrame::get_base_name(const wxString full_name) const 
{
    return boost::filesystem::path(full_name).filename().string();
}

std::string MainFrame::get_dir_name(const wxString full_name) const 
{
    return boost::filesystem::path(full_name).parent_path().string();
}


} // GUI
} // Slic3r
