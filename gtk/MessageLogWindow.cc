/*
 * This file Copyright (C) 2008-2021 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <glibmm.h>
#include <glibmm/i18n.h>

#include <libtransmission/transmission.h>
#include <libtransmission/log.h>

#include "Actions.h"
#include "HigWorkarea.h"
#include "MessageLogWindow.h"
#include "Prefs.h"
#include "PrefsDialog.h"
#include "Session.h"
#include "Utils.h"

class MessageLogColumnsModel : public Gtk::TreeModelColumnRecord
{
public:
    MessageLogColumnsModel()
    {
        add(sequence);
        add(name);
        add(message);
        add(tr_msg);
    }

    Gtk::TreeModelColumn<unsigned int> sequence;
    Gtk::TreeModelColumn<Glib::ustring> name;
    Gtk::TreeModelColumn<Glib::ustring> message;
    Gtk::TreeModelColumn<tr_log_message*> tr_msg;
};

MessageLogColumnsModel const message_log_cols;

class MessageLogWindow::Impl
{
public:
    Impl(MessageLogWindow& window, Glib::RefPtr<Session> const& core);
    ~Impl();

private:
    bool onRefresh();

    void onSaveRequest();
    void onSaveDialogResponse(Gtk::FileChooserDialog* d, int response);
    void doSave(Gtk::Window* parent, Glib::ustring const& filename);

    void onClearRequest();
    void onPauseToggled(Gtk::ToggleToolButton* w);

    void scroll_to_bottom();
    void level_combo_changed_cb(Gtk::ComboBox* combo_box);

    bool is_pinned_to_new() const;
    bool isRowVisible(Gtk::TreeModel::const_iterator const& iter) const;

private:
    MessageLogWindow& window_;

    Glib::RefPtr<Session> const core_;
    Gtk::TreeView* view_ = nullptr;
    Glib::RefPtr<Gtk::ListStore> store_;
    Glib::RefPtr<Gtk::TreeModelFilter> filter_;
    Glib::RefPtr<Gtk::TreeModelSort> sort_;
    tr_log_level maxLevel_ = TR_LOG_SILENT;
    bool isPaused_ = false;
    sigc::connection refresh_tag_;
};

namespace
{

tr_log_message* myTail = nullptr;
tr_log_message* myHead = nullptr;

} // namespace

/****
*****
****/

/* is the user looking at the latest messages? */
bool MessageLogWindow::Impl::is_pinned_to_new() const
{
    bool pinned_to_new = false;

    if (view_ == nullptr)
    {
        pinned_to_new = true;
    }
    else
    {
        Gtk::TreeModel::Path first_visible;
        Gtk::TreeModel::Path last_visible;

        if (view_->get_visible_range(first_visible, last_visible))
        {
            auto const row_count = sort_->children().size();

            if (auto const iter = sort_->children()[row_count - 1]; iter)
            {
                pinned_to_new = last_visible == sort_->get_path(iter);
            }
        }
    }

    return pinned_to_new;
}

void MessageLogWindow::Impl::scroll_to_bottom()
{
    if (sort_ != nullptr)
    {
        auto const row_count = sort_->children().size();

        if (auto const iter = sort_->children()[row_count - 1]; iter)
        {
            view_->scroll_to_row(sort_->get_path(iter), 1);
        }
    }
}

/****
*****
****/

void MessageLogWindow::Impl::level_combo_changed_cb(Gtk::ComboBox* combo_box)
{
    auto const level = static_cast<tr_log_level>(gtr_combo_box_get_active_enum(*combo_box));
    bool const pinned_to_new = is_pinned_to_new();

    tr_logSetLevel(level);
    core_->set_pref(TR_KEY_message_level, level);
    maxLevel_ = level;
    filter_->refilter();

    if (pinned_to_new)
    {
        scroll_to_bottom();
    }
}

namespace
{

/* similar to asctime, but is utf8-clean */
Glib::ustring gtr_asctime(time_t t)
{
    return Glib::DateTime::create_now_local(t).format("%a %b %e %T %Y"); /* ctime equiv */
}

} // namespace

void MessageLogWindow::Impl::doSave(Gtk::Window* parent, Glib::ustring const& filename)
{
    auto* fp = fopen(filename.c_str(), "w+");

    if (fp == nullptr)
    {
        auto* w = new Gtk::MessageDialog(
            *parent,
            gtr_sprintf(_("Couldn't save \"%s\""), filename),
            false,
            Gtk::MESSAGE_ERROR,
            Gtk::BUTTONS_CLOSE);
        w->set_secondary_text(Glib::strerror(errno));
        w->signal_response().connect([w](int /*response*/) { delete w; });
        w->show();
    }
    else
    {
        for (auto const& row : store_->children())
        {
            char const* levelStr;
            auto const* const node = row.get_value(message_log_cols.tr_msg);

            auto const date = gtr_asctime(node->when);

            switch (node->level)
            {
            case TR_LOG_DEBUG:
                levelStr = "debug";
                break;

            case TR_LOG_ERROR:
                levelStr = "error";
                break;

            default:
                levelStr = "     ";
                break;
            }

            fprintf(
                fp,
                "%s\t%s\t%s\t%s\n",
                date.c_str(),
                levelStr,
                node->name != nullptr ? node->name : "",
                node->message != nullptr ? node->message : "");
        }

        fclose(fp);
    }
}

void MessageLogWindow::Impl::onSaveDialogResponse(Gtk::FileChooserDialog* d, int response)
{
    if (response == Gtk::RESPONSE_ACCEPT)
    {
        doSave(d, d->get_filename());
    }

    delete d;
}

void MessageLogWindow::Impl::onSaveRequest()
{
    auto* d = new Gtk::FileChooserDialog(window_, _("Save Log"), Gtk::FILE_CHOOSER_ACTION_SAVE);
    d->add_button(_("_Cancel"), Gtk::RESPONSE_CANCEL);
    d->add_button(_("_Save"), Gtk::RESPONSE_ACCEPT);

    d->signal_response().connect([this, d](int response) { onSaveDialogResponse(d, response); });
    d->show();
}

void MessageLogWindow::Impl::onClearRequest()
{
    store_->clear();
    tr_logFreeQueue(myHead);
    myHead = myTail = nullptr;
}

void MessageLogWindow::Impl::onPauseToggled(Gtk::ToggleToolButton* w)
{
    isPaused_ = w->get_active();
}

namespace
{

void setForegroundColor(Gtk::CellRendererText* renderer, int msgLevel)
{
    if (msgLevel == TR_LOG_DEBUG)
    {
        renderer->property_foreground() = "forestgreen";
    }
    else if (msgLevel == TR_LOG_ERROR)
    {
        renderer->property_foreground() = "red";
    }
    else
    {
        renderer->property_foreground_set() = false;
    }
}

void renderText(
    Gtk::CellRendererText* renderer,
    Gtk::TreeModel::iterator const& iter,
    Gtk::TreeModelColumn<Glib::ustring> const& col)
{
    auto const* const node = iter->get_value(message_log_cols.tr_msg);
    renderer->property_text() = iter->get_value(col);
    renderer->property_ellipsize() = Pango::ELLIPSIZE_END;
    setForegroundColor(renderer, node->level);
}

void renderTime(Gtk::CellRendererText* renderer, Gtk::TreeModel::iterator const& iter)
{
    auto const* const node = iter->get_value(message_log_cols.tr_msg);
    renderer->property_text() = Glib::DateTime::create_now_local(node->when).format("%T");
    setForegroundColor(renderer, node->level);
}

void appendColumn(Gtk::TreeView* view, Gtk::TreeModelColumnBase const& col)
{
    Gtk::TreeViewColumn* c;

    if (col == message_log_cols.name)
    {
        auto* r = Gtk::make_managed<Gtk::CellRendererText>();
        c = Gtk::make_managed<Gtk::TreeViewColumn>(_("Name"), *r);
        c->set_cell_data_func(*r, [r](auto* /*renderer*/, auto const& iter) { renderText(r, iter, message_log_cols.name); });
        c->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
        c->set_fixed_width(200);
        c->set_resizable(true);
    }
    else if (col == message_log_cols.message)
    {
        auto* r = Gtk::make_managed<Gtk::CellRendererText>();
        c = Gtk::make_managed<Gtk::TreeViewColumn>(_("Message"), *r);
        c->set_cell_data_func(*r, [r](auto* /*renderer*/, auto const& iter) { renderText(r, iter, message_log_cols.message); });
        c->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
        c->set_fixed_width(500);
        c->set_resizable(true);
    }
    else if (col == message_log_cols.sequence)
    {
        auto* r = Gtk::make_managed<Gtk::CellRendererText>();
        c = Gtk::make_managed<Gtk::TreeViewColumn>(_("Time"), *r);
        c->set_cell_data_func(*r, [r](auto* /*renderer*/, auto const& iter) { renderTime(r, iter); });
        c->set_resizable(true);
    }
    else
    {
        g_assert_not_reached();
    }

    view->append_column(*c);
}

} // namespace

bool MessageLogWindow::Impl::isRowVisible(Gtk::TreeModel::const_iterator const& iter) const
{
    auto const* const node = iter->get_value(message_log_cols.tr_msg);
    return node != nullptr && node->level <= maxLevel_;
}

MessageLogWindow::Impl::~Impl()
{
    refresh_tag_.disconnect();
}

namespace
{

tr_log_message* addMessages(Glib::RefPtr<Gtk::ListStore> const& store, tr_log_message* head)
{
    tr_log_message* i;
    static unsigned int sequence = 0;
    auto const default_name = Glib::get_application_name();

    for (i = head; i != nullptr && i->next != nullptr; i = i->next)
    {
        char const* name = i->name != nullptr ? i->name : default_name.c_str();

        auto const row = *store->prepend();
        row[message_log_cols.tr_msg] = i;
        row[message_log_cols.name] = name;
        row[message_log_cols.message] = i->message;
        row[message_log_cols.sequence] = ++sequence;

        /* if it's an error message, dump it to the terminal too */
        if (i->level == TR_LOG_ERROR)
        {
            auto gstr = gtr_sprintf("%s:%d %s", i->file, i->line, i->message);

            if (i->name != nullptr)
            {
                gstr += gtr_sprintf(" (%s)", i->name);
            }

            g_warning("%s", gstr.c_str());
        }
    }

    return i; /* tail */
}

} // namespace

bool MessageLogWindow::Impl::onRefresh()
{
    bool const pinned_to_new = is_pinned_to_new();

    if (!isPaused_)
    {
        auto* msgs = tr_logGetQueue();

        if (msgs != nullptr)
        {
            /* add the new messages and append them to the end of
             * our persistent list */
            tr_log_message* tail = addMessages(store_, msgs);

            if (myTail != nullptr)
            {
                myTail->next = msgs;
            }
            else
            {
                myHead = msgs;
            }

            myTail = tail;
        }

        if (pinned_to_new)
        {
            scroll_to_bottom();
        }
    }

    return true;
}

namespace
{

Gtk::ComboBox* debug_level_combo_new()
{
    auto* w = gtr_combo_box_new_enum({
        { _("Error"), TR_LOG_ERROR },
        { _("Information"), TR_LOG_INFO },
        { _("Debug"), TR_LOG_DEBUG },
    });
    gtr_combo_box_set_active_enum(*w, gtr_pref_int_get(TR_KEY_message_level));
    return w;
}

} // namespace

/**
***  Public Functions
**/

std::unique_ptr<MessageLogWindow> MessageLogWindow::create(Gtk::Window& parent, Glib::RefPtr<Session> const& core)
{
    return std::unique_ptr<MessageLogWindow>(new MessageLogWindow(parent, core));
}

MessageLogWindow::MessageLogWindow(Gtk::Window& parent, Glib::RefPtr<Session> const& core)
    : Gtk::Window(Gtk::WINDOW_TOPLEVEL)
    , impl_(std::make_unique<Impl>(*this, core))
{
    set_transient_for(parent);
}

MessageLogWindow::~MessageLogWindow() = default;

MessageLogWindow::Impl::Impl(MessageLogWindow& window, Glib::RefPtr<Session> const& core)
    : window_(window)
    , core_(core)
{
    window_.set_title(_("Message Log"));
    window_.set_default_size(560, 350);
    window_.set_role("message-log");
    auto* vbox = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);

    /**
    ***  toolbar
    **/

    auto* toolbar = Gtk::make_managed<Gtk::Toolbar>();
    toolbar->set_toolbar_style(Gtk::TOOLBAR_BOTH_HORIZ);
    toolbar->get_style_context()->add_class(GTK_STYLE_CLASS_PRIMARY_TOOLBAR);

    {
        auto* icon = Gtk::make_managed<Gtk::Image>();
        icon->set_from_icon_name("document-save-as", Gtk::BuiltinIconSize::ICON_SIZE_SMALL_TOOLBAR);
        auto* item = Gtk::make_managed<Gtk::ToolButton>(*icon);
        item->set_is_important(true);
        item->set_label(_("Save _As"));
        item->set_use_underline(true);
        item->signal_clicked().connect(sigc::mem_fun(*this, &Impl::onSaveRequest));
        toolbar->insert(*item, -1);
    }

    {
        auto* icon = Gtk::make_managed<Gtk::Image>();
        icon->set_from_icon_name("edit-clear", Gtk::BuiltinIconSize::ICON_SIZE_SMALL_TOOLBAR);
        auto* item = Gtk::make_managed<Gtk::ToolButton>(*icon);
        item->set_is_important(true);
        item->set_label(_("Clear"));
        item->set_use_underline(true);
        item->signal_clicked().connect(sigc::mem_fun(*this, &Impl::onClearRequest));
        toolbar->insert(*item, -1);
    }

    toolbar->insert(*Gtk::make_managed<Gtk::SeparatorToolItem>(), -1);

    {
        auto* icon = Gtk::make_managed<Gtk::Image>();
        icon->set_from_icon_name("media-playback-pause", Gtk::BuiltinIconSize::ICON_SIZE_SMALL_TOOLBAR);
        auto* item = Gtk::make_managed<Gtk::ToggleToolButton>(*icon);
        item->set_is_important(true);
        item->set_label(_("P_ause"));
        item->set_use_underline(true);
        item->signal_toggled().connect([this, item]() { onPauseToggled(item); });
        toolbar->insert(*item, -1);
    }

    toolbar->insert(*Gtk::make_managed<Gtk::SeparatorToolItem>(), -1);

    {
        auto* w = Gtk::make_managed<Gtk::Label>(_("Level"));
        w->property_margin() = GUI_PAD;
        auto* item = Gtk::make_managed<Gtk::ToolItem>();
        item->add(*w);
        toolbar->insert(*item, -1);
    }

    {
        auto* w = debug_level_combo_new();
        w->signal_changed().connect([this, w]() { level_combo_changed_cb(w); });
        auto* item = Gtk::make_managed<Gtk::ToolItem>();
        item->add(*w);
        toolbar->insert(*item, -1);
    }

    vbox->pack_start(*toolbar, false, false, 0);

    /**
    ***  messages
    **/

    store_ = Gtk::ListStore::create(message_log_cols);

    addMessages(store_, myHead);
    onRefresh(); /* much faster to populate *before* it has listeners */

    filter_ = Gtk::TreeModelFilter::create(store_);
    sort_ = Gtk::TreeModelSort::create(filter_);
    sort_->set_sort_column(message_log_cols.sequence, Gtk::SORT_ASCENDING);
    maxLevel_ = static_cast<tr_log_level>(gtr_pref_int_get(TR_KEY_message_level));
    filter_->set_visible_func(sigc::mem_fun(*this, &Impl::isRowVisible));

    view_ = Gtk::make_managed<Gtk::TreeView>(sort_);
    view_->signal_button_release_event().connect([this](GdkEventButton* event)
                                                 { return on_tree_view_button_released(view_, event); });
    appendColumn(view_, message_log_cols.sequence);
    appendColumn(view_, message_log_cols.name);
    appendColumn(view_, message_log_cols.message);
    auto* w = Gtk::make_managed<Gtk::ScrolledWindow>();
    w->set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    w->set_shadow_type(Gtk::SHADOW_IN);
    w->add(*view_);
    vbox->pack_start(*w, true, true, 0);
    window_.add(*vbox);

    refresh_tag_ = Glib::signal_timeout().connect_seconds(
        sigc::mem_fun(*this, &Impl::onRefresh),
        SECONDARY_WINDOW_REFRESH_INTERVAL_SECONDS);

    scroll_to_bottom();
    window_.show_all_children();
}

void MessageLogWindow::on_show()
{
    Gtk::Window::on_show();
    gtr_action_set_toggled("toggle-message-log", true);
}

void MessageLogWindow::on_hide()
{
    Gtk::Window::on_hide();
    gtr_action_set_toggled("toggle-message-log", false);
}
