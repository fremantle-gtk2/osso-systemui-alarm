#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libintl.h>
#include <locale.h>

#include <glib.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gconf/gconf-client.h>
#include <pango/pango.h>
#include <dbus/dbus.h>

#include <libalarm.h>
#include <libnotify/notification.h>
#include <libnotify/notify.h>
#include <hildon/hildon-helper.h>
#include <hildon/hildon-note.h>

#include <clockd/libtime.h>

#include "systemui.h"


static gboolean snooze_timeout(gpointer user_data);
static DBusHandlerResult dbus_filter(DBusConnection *connection, DBusMessage *message, void *user_data);

static void clicked_cb(GtkWidget *widget, gpointer user_data);
static gboolean calendar_timeout(gpointer user_data);

static void show_all_alarms();

struct alarm
{
  cookie_t cookie;
  alarm_event_t *alarm_event;
  gboolean pending;
  guint event_index;
  int snooze_cnt;
  int app_id;
  int snooze_action_index;
  guint calendar_timeout_tag;
  guint snooze_timeout_tag;
  gboolean has_dbus_filter;
  NotifyNotification *notification;
  GSList *buttons;
};

enum {
  UP,
  DOWN
}device_orientation;

guint last_event_index = 0;
gboolean act_dead = FALSE;
int window_priority = 190;

GtkWidget *alarm_dialog = NULL;
GtkWidget *alarm_hbox = NULL;
GtkWidget *alarm_vbox = NULL;
GtkWidget *dialog_button_box = NULL;

guint idle_tag = 0;

guint alarm_events_cnt = 0;

struct systemui *system_ui_info = NULL;

void *plugin = 0;
void *id = 0;

GSList* alarms = NULL;


#define ALARM_TARGET_TIME_OFFSET "target_time_offset"
#define ALARM_CALENDAR_EVENT_TYPE "calendar_event_type"
#define ALARM_FOR_NORMAL_EVENT "timedevent"
#define ALARM_FOR_ALLDAY "allday"
#define ALARM_FOR_TASK "task"
#define ALARM_FOR_BIRTHDAY "birthday"

void *notify_actdead_init()
{
  void *rv;

  rv = &plugin;
  if ( !plugin )
    rv = nsv_sv_init(&plugin);
  return rv;
}

void *notify_actdead_shutdown()
{
  void *rv;

  rv = plugin;
  if ( plugin )
  {
    rv = nsv_sv_shutdown(plugin);
    plugin = 0;
  }
  return rv;
}

static void add_alarm(cookie_t cookie)
{
  struct alarm* a;

  a = calloc(1, sizeof(struct alarm));

  a->cookie = cookie;
  a->pending = 1;
  a->event_index = -1;
  a->app_id = -1;
  a->snooze_action_index = -1;

  alarm_events_cnt++;

  alarms = g_slist_append(alarms,a);
}

static struct alarm *find_alarm(cookie_t cookie)
{
  GSList *i = NULL;

  for( i = alarms; i; i = i->next )
  {
    if( ((struct alarm*)i->data)->cookie == cookie)
      return (struct alarm*)i->data;
  }

  return NULL;
}

static void remove_alarm(cookie_t cookie)
{
  GSList *i = NULL;
  struct alarm* a;

  a = find_alarm(cookie);

  if(a)
  {
    alarm_event_delete(a->alarm_event);
    g_slist_free(a->buttons);
    free(a);
    alarms = g_slist_remove(alarms,a);
    alarm_events_cnt--;
  }
}

static void remove_unneeded_alarms(gboolean act_dead)
{
  GSList *i= NULL;
  struct alarm* a;

  if ( act_dead )
  {

    for( i = alarms; i; i = i->next )
    {
      if ( ((struct alarm*)i->data)->pending )
        act_dead = 0;
    }
  }

  i = alarms;

  while( i )
  {
    struct alarm *a = ((struct alarm*)i->data);

    if ( !a->pending && !act_dead )
    {
      alarmd_ack_dialog(a->cookie, last_event_index | a->event_index);
      remove_alarm(a->cookie);
      i = alarms;
      act_dead = 0;

      continue;
    }

    act_dead = 0;
    i  = i->next;
  }
}



static void response_cb(GtkDialog *dialog, gint response_id, gpointer user_data)
{
  if ( response_id == GTK_RESPONSE_DELETE_EVENT )
    g_signal_stop_emission_by_name(dialog, "response");
}

static gboolean key_press_cb(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  return TRUE;
}

static void widget_destroy(GtkWidget *widget, gpointer data)
{
  gtk_widget_destroy(widget);
}

static void size_request_cb(GtkWidget *widget, GtkRequisition *requisition, gpointer user_data)
{
  PangoFontMetrics *metrics;
  int height;
  PangoLayout *layout;

  metrics = pango_context_get_metrics(gtk_widget_get_pango_context(widget),
                                      gtk_widget_get_style(widget)->font_desc, 0);

  /* check me */
  height = (2 * (pango_font_metrics_get_descent(metrics) + pango_font_metrics_get_ascent(metrics))) / PANGO_SCALE;

  if ( height < requisition->height )
    requisition->height = height;
  if ( requisition->width > 590 )
    requisition->width = 590;

  layout = gtk_label_get_layout((GtkLabel *)widget);

  pango_layout_set_wrap(layout, PANGO_WRAP_WORD);
  pango_layout_set_width(layout, requisition->width * PANGO_SCALE );
  pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
  pango_layout_set_height(layout, requisition->height * PANGO_SCALE);
}

static gchar *time2str(struct tm *tm)
{
  const char *msgid;
  char time_buf[256];

  if ( gconf_client_get_bool(system_ui_info->client, "/apps/clock/time-format", 0) )
    msgid = "wdgt_va_24h_time";
  else
    msgid = tm->tm_hour > 11 ? "wdgt_va_12h_time_pm" : "wdgt_va_12h_time_am";

  time_format_time(tm,
                   dgettext("hildon-libs", msgid),
                   time_buf, sizeof(time_buf));

  return g_strndup(time_buf, sizeof(time_buf));
}

gboolean idle_func(gpointer user_data)
{
  cookie_t *cookies;
  int i;

  cookies = calloc(alarm_events_cnt + 1, sizeof(cookie_t *));

  for(i = 0; i < alarm_events_cnt; i++)
  {
    struct alarm * a = (struct alarm *)(g_slist_nth(alarms,i)->data);
    cookies[i] = a->cookie;
  }

  alarmd_ack_queue(cookies, alarm_events_cnt);
  free(cookies);
  idle_tag = 0;
  return FALSE;
}

static guint add_idle_func()
{
  if ( !idle_tag )
  {
    idle_tag = g_idle_add(idle_func, 0);
  }
  return idle_tag;
}

static int alarm_open(const char *interface, const char *method, GArray *param, struct systemui *sui, int *out)
{
  cookie_t cookie;
  struct alarm *a;

  system_ui_info = sui;

  /* FIXME */
  cookie = *(cookie_t*)(param->data+8) & INT_MAX;

  if ( !find_alarm(cookie) )
    add_alarm(cookie);

  /* FIXME */
  if ( *(cookie_t*)(param->data+8) >= 0 )
  {
    show_all_alarms();
    add_idle_func();
  }

  out[0] = 'i';
  out[1] = 1;

  return 'i';
}

static int alarm_close(const char *interface, const char *method, GArray *param, struct systemui *sui, int *out)
{
  remove_alarm(g_array_index(param,int,1) & INT_MAX);

  if ( g_array_index(param,int,1)  >= 0 )
  {
    show_all_alarms();
    add_idle_func();
  }

  out[0] = 'i';
  out[1] = 1;
  return 'i';
}

int plugin_init(struct systemui *sui)
{
  DBusError error = DBUS_ERROR_INIT;

  window_priority = gconf_client_get_int(sui->client, "/system/systemui/alarm/window_priority", 0);

  if ( !window_priority )
    window_priority = 190;

  if (!act_dead && !access("/tmp/ACT_DEAD", 4))
  {
    act_dead = TRUE;
    notify_actdead_init();
  }

  dbus_bus_add_match(sui->dbus,
                     "type='signal',interface='com.nokia.mce.signal',path='/com/nokia/mce/signal'",
                     &error);

  if(dbus_error_is_set(&error))
    dbus_error_free(&error);

  add_handler("alarm_open", alarm_open, sui);
  add_handler("alarm_close", alarm_close, sui);

  return TRUE;
}


void plugin_close(struct systemui *sui)
{
  DBusError error = DBUS_ERROR_INIT;

/* WTF? Why there is a check in plugin_init, but act_dead is not reset here? */
  if ( act_dead )
    notify_actdead_shutdown();

  dbus_bus_remove_match(
    sui->dbus,
    "type='signal',interface='com.nokia.mce.signal',path='/com/nokia/mce/signal'",
    &error);

  if(dbus_error_is_set(&error))
    dbus_error_free(&error);

  remove_handler("alarm_open", sui);
  remove_handler("alarm_close", sui);

  WindowPriority_HideWindow(alarm_dialog);

  if ( alarm_dialog )
    gtk_object_destroy(GTK_OBJECT(alarm_dialog));
}

static gboolean dbus_send_alarm_dialog_status(dbus_int32_t status)
{
  DBusMessage *message;
  gboolean result = FALSE;

  message = dbus_message_new_signal("/com/nokia/system_ui/signal",
                                    "com.nokia.system_ui.signal",
                                    "alarm_dialog_status");

  if ( message )
  {
    if ( dbus_message_append_args(message,
                                  DBUS_TYPE_UINT32, &status,
                                  DBUS_TYPE_INVALID) )
    {
      dbus_message_set_no_reply(message, TRUE);

      if( dbus_connection_send(system_ui_info->dbus, message, NULL) )
      {
        dbus_connection_flush(system_ui_info->dbus);
        result = TRUE;
      }
    }

    dbus_message_unref(message);
  }

  return result;
}

static alarm_event_t *get_alarm_event(struct alarm *a)
{
  if (!a)
    return NULL;

  if( !a->alarm_event )
    a->alarm_event = alarmd_event_get(a->cookie);

  return a->alarm_event;
}

static void stop_timeout_for_alarm(struct alarm *a)
{
  if ( a->calendar_timeout_tag > 0 )
  {
    g_source_remove(a->calendar_timeout_tag);
    a->calendar_timeout_tag = 0;
  }
  if ( a->snooze_timeout_tag > 0 )
  {
    g_source_remove(a->snooze_timeout_tag);
    a->snooze_timeout_tag = 0;
  }
}

static void stop_timeouts(struct alarm *a)
{
  GSList * i;
  if(a)
    stop_timeout_for_alarm(a);
  else
  {
    for(i = alarms; i; i = i->next)
    {
      stop_timeout_for_alarm((struct alarm *)i->data);
    }
  }
}

gboolean notify_alarm_stop(NotifyNotification *n)
{
  GError *error = NULL;

  if ( n && !notify_notification_close(n, &error) )
    g_error_free(error);

  if ( plugin )
    nsv_sv_stop_event(plugin, id);

  return 1;
}

static gboolean accelerometer_disable(cookie_t cookie)
{
  DBusMessage *message;
  struct alarm * a;

  if(cookie != -1)
  {
    if( (a = find_alarm(cookie)) )
    {
      if( a->notification )
      {
        notify_alarm_stop(a->notification);
        a->notification = NULL;
      }

      if(a->has_dbus_filter)
      {
        dbus_connection_remove_filter(system_ui_info->dbus, dbus_filter, (gpointer)cookie);
        a->has_dbus_filter = FALSE;
      }
    }
  }
  else
  {
    GSList *i;

    for(i = alarms; i; i = i->next)
    {
      a = (struct alarm *)i->data;
      if(a->has_dbus_filter)
      {
        dbus_connection_remove_filter(system_ui_info->dbus, dbus_filter, (gpointer)a->cookie);
        a->has_dbus_filter = FALSE;
      }
    }
  }

  message = dbus_message_new_method_call(
              "com.nokia.mce",
              "/com/nokia/mce/request",
              "com.nokia.mce.request",
              "req_accelerometer_disable");

  dbus_message_set_no_reply(message, TRUE);
  dbus_connection_send(system_ui_info->dbus, message, NULL);
  dbus_connection_flush(system_ui_info->dbus);
  dbus_message_unref(message);

  return FALSE;
}

static DBusHandlerResult dbus_filter(DBusConnection *connection, DBusMessage *message, void *user_data)
{
  DBusError error = DBUS_ERROR_INIT;
  int type;
  const char *iface;
  const char *member;
  const char *path;

  int x,y,z;
  gchar *orientation,*stand,*face;

  iface = dbus_message_get_interface(message);
  member = dbus_message_get_member(message);
  path = dbus_message_get_path(message);
  type = dbus_message_get_type(message);

  if ( iface && member && path && type == DBUS_MESSAGE_TYPE_SIGNAL )
  {

    if(!strcmp(iface,"com.nokia.mce.signal") &&
       !strcmp(path,"/com/nokia/mce/signal") &&
       !strcmp(member,"sig_device_orientation_ind"))
    {
      dbus_message_get_args( message, &error,
                             DBUS_TYPE_STRING, &orientation,
                             DBUS_TYPE_STRING, &stand,
                             DBUS_TYPE_STRING, &face,
                             DBUS_TYPE_INT32, &x,
                             DBUS_TYPE_INT32, &y,
                             DBUS_TYPE_INT32, &z,
                             DBUS_TYPE_INVALID);
      if ( face )
      {
        if ( !strcmp(face,"face_down") && !device_orientation && alarm_dialog )
        {
          clicked_cb(alarm_dialog, user_data);
          device_orientation = DOWN;
        }
        else if ( !strcmp(face,"face_up") )
          device_orientation = UP;
      }
    }
  }
  return 1;
}

static void clicked_cb(GtkWidget *widget, gpointer user_data)
{
  struct alarm *a;

  accelerometer_disable((cookie_t)user_data);

  a = find_alarm((cookie_t)user_data);

  if ( a )
  {
    stop_timeouts(a);
    alarm_event_t *ae = get_alarm_event(a);
    if ( ae )
    {
      if ( !strcmp(alarm_action_get_label(alarm_event_get_action(ae, a->snooze_action_index)), "cloc_bd_view") )
      {
        gchar * s = "unlocked";
        DBusMessage *message;

        message = dbus_message_new_method_call(
                "com.nokia.mce",
                "/com/nokia/mce/request",
                "com.nokia.mce.request",
                "req_tklock_mode_change");

        dbus_message_append_args(message,
                                 DBUS_TYPE_STRING, s,
                                 DBUS_TYPE_INVALID);

        dbus_message_set_no_reply(message, TRUE);
        dbus_connection_send(system_ui_info->dbus, message, NULL);
        dbus_connection_flush(system_ui_info->dbus);
        dbus_message_unref(message);
      }
      a->pending = FALSE;
      a->event_index = g_slist_index(a->buttons,widget);
      if(a->event_index == -1)
        a->event_index = a->snooze_action_index;
    }
  }

  show_all_alarms();
}

NotifyNotification *notify_alarm_start_calendar(const char *sound_file)
{
  NotifyNotification *n;
  GError *error = NULL;

  if ( sound_file && (notify_is_initted() || notify_init("notify-calendar-alarm")) )
  {
    n = notify_notification_new("alarm", 0, "/usr/share/icons/hicolor/10x10/hildon/qgn_list_smiley_angry.png", 0);
    notify_notification_set_category(n, "alarm-event");
    notify_notification_set_timeout(n, 0);
    notify_notification_set_hint_string(n, "alarm-type", "calendar");
    notify_notification_set_hint_int32(n, "volume", 100);
    notify_notification_set_hint_string(n, "sound-file", sound_file);
    notify_notification_set_hint_string(n, "vibra", "PatternIncomingCall");

    if ( !notify_notification_show(n, &error) )
    {
      g_error_free(error);
      n = NULL;
    }
    return n;
  }

  return NULL;
}

NotifyNotification *notify_alarm_start_clock(const char *sound_file)
{
  NotifyNotification *n;
  GError *error = NULL;

  if ( sound_file )
  {
    if ( access("/tmp/ACT_DEAD", 4) )
    {
      if ( !notify_is_initted() && !notify_init("notify-clock-alarm") )
        return NULL;

      n = notify_notification_new("alarm", 0, "/usr/share/icons/hicolor/10x10/hildon/qgn_list_smiley_angry.png", NULL);
      notify_notification_set_category(n, "alarm-event");
      notify_notification_set_timeout(n, 0);
      notify_notification_set_hint_string(n, "alarm-type", "clock");
      notify_notification_set_hint_int32(n, "volume", 100);
      notify_notification_set_hint_string(n, "sound-file", sound_file);
      notify_notification_set_hint_string(n, "vibra", "PatternIncomingCall");

      if ( !notify_notification_show(n, &error) )
      {
        g_error_free(error);
        n = NULL;
      }
      return n;
    }
    if ( plugin )
    {
      id = nsv_sv_play_event(plugin);
      return NULL;
    }
  }
  return NULL;
}

static int get_alarm_event_application(struct alarm *a)
{
  alarm_event_t *event;

  if ( !a )
    return 0;

  if ( a->app_id == -1 )
  {
    a->app_id = 0;
    event = get_alarm_event(a);

    if ( event && event->alarm_appid )
    {
      if(!strcmp(event->alarm_appid, "worldclock_alarmd_id"))
        a->app_id = 1;
      else if(!strcmp(event->alarm_appid, "Calendar"))
          a->app_id = 2;
    }
  }

  return a->app_id;
}

static gboolean is_calendar(struct alarm *a)
{
  return get_alarm_event_application(a) == 2;
}

static void alarm_notify(cookie_t cookie)
{
  DBusMessage *message;
  DBusMessage  *reply;
  struct alarm *a;
  gchar * sound_file;


  a = find_alarm(cookie);
  if ( a )
  {
    stop_timeouts(a);
    DBusError error = DBUS_ERROR_INIT;
    int x,y,z;
    gchar *orientation,*stand,*face;

    message = dbus_message_new_method_call(
           "com.nokia.mce",
           "/com/nokia/mce/request",
           "com.nokia.mce.request",
           "req_accelerometer_enable");

    dbus_message_set_no_reply(message, TRUE);
    dbus_connection_send(system_ui_info->dbus, message, NULL);
    dbus_connection_flush(system_ui_info->dbus);
    dbus_message_unref(message);

    dbus_connection_add_filter(system_ui_info->dbus, dbus_filter, (void*)cookie, NULL);
    a->has_dbus_filter = TRUE;

    message = dbus_message_new_method_call(
           "com.nokia.mce",
           "/com/nokia/mce/request",
           "com.nokia.mce.request",
           "get_device_orientation");

    reply = dbus_connection_send_with_reply_and_block(system_ui_info->dbus, message, -1, &error);

    if ( reply )
    {
      dbus_message_get_args( reply, &error,
                             DBUS_TYPE_STRING, &orientation,
                             DBUS_TYPE_STRING, &stand,
                             DBUS_TYPE_STRING, &face,
                             DBUS_TYPE_INT32, &x,
                             DBUS_TYPE_INT32, &y,
                             DBUS_TYPE_INT32, &z,
                             DBUS_TYPE_INVALID);

      device_orientation = (!strcmp(orientation,"face_down"))? DOWN : UP;
      dbus_message_unref(reply);
    }

    dbus_message_unref(message);

    if(dbus_error_is_set(&error))
      dbus_error_free(&error);

    if ( is_calendar(a) )
    {
      gchar * s = gconf_client_get_string(system_ui_info->client, "/apps/calendar/calendar-alarm-tone", NULL);

      sound_file = g_strdup(s);
      g_free(s);

      a->notification = notify_alarm_start_calendar(sound_file);
    }
    else
    {
      gchar * s = gconf_client_get_string(system_ui_info->client, "/apps/clock/alarm-tone", NULL);

      sound_file = g_strdup(s);
      g_free(s);

      if ( access(sound_file, 4) == -1 )
      {
        g_free(sound_file);
        sound_file = g_strdup("/usr/share/sounds/ui-clock_alarm_default.aac");
      }

      a->notification = notify_alarm_start_clock(sound_file);
    }

    g_free(sound_file);

    a->calendar_timeout_tag = g_timeout_add_seconds(is_calendar(a) < 1 ? 60 : 10, calendar_timeout, (gpointer)a->cookie);

    if ( a->snooze_cnt < 3 )
      a->snooze_timeout_tag = g_timeout_add_seconds(300, snooze_timeout, (gpointer)a->cookie);

    dbus_send_alarm_dialog_status(5);

    message = dbus_message_new_method_call(
            "com.nokia.mce",
            "/com/nokia/mce/request",
            "com.nokia.mce.request",
            "req_display_state_on");
    dbus_message_set_no_reply(message, TRUE);
    dbus_connection_send(system_ui_info->dbus, message, 0);
    dbus_connection_flush(system_ui_info->dbus);
    dbus_message_unref(message);
  }
}

static gboolean snooze_timeout(gpointer user_data)
{
  struct alarm *a;

  a->snooze_timeout_tag = 0;
  a = find_alarm((cookie_t)user_data);

  if ( a )
  {
    ++a->snooze_cnt;
    alarm_notify((cookie_t)user_data);
  }

  return 0;
}

static time_t  get_alarm_time(alarm_event_t *event)
{
  time_t rv;
  time_t tm_off;

  rv = alarm_event_get_attr_time(event, "target_time", 0);
  tm_off = alarm_event_get_attr_time(event, ALARM_TARGET_TIME_OFFSET, 0);

  if ( !rv )
    rv = tm_off + event->snooze_total?event->snooze_total:event->trigger;

  return rv;
}

static gchar *get_full_date(const struct tm *t)
{
  char buf[256];

  time_format_time(t,
                   dgettext("hildon-libs", "wdgt_va_fulldate_day_name_short"),
                   buf, sizeof(buf));

  return g_strndup(buf, sizeof(buf));
}

static void set_label_time_text(GtkWidget * label,alarm_event_t *alarm_event)
{
  time_t t = get_alarm_time(alarm_event);
  if ( t )
  {
    struct tm lt;
    gchar *s;
    time_get_synced();
    time_get_local_ex(t, &lt);
    s = get_full_date(&lt);
    gtk_label_set_text(GTK_LABEL(label), s);
    hildon_helper_set_logical_font(label, "LargeSystemFont");
    g_free(s);
  }
}

gboolean show_alarm_dialog(struct alarm *a)
{
  alarm_event_t *alarm_event;
  char *ptr;
  GtkWidget *time_label;
  int i;

  alarm_event = get_alarm_event(a);

  if ( !alarm_event )
    return FALSE;

  accelerometer_disable(a->cookie);

  if ( !alarm_dialog )
  {
    alarm_dialog = gtk_dialog_new();
    if(!alarm_dialog)
      return FALSE;

    gtk_widget_set_no_show_all(GTK_DIALOG(alarm_dialog)->action_area, TRUE);
    gtk_widget_hide(GTK_DIALOG(alarm_dialog)->action_area);
    gtk_window_set_default_size(GTK_WINDOW(alarm_dialog), 840, 348);

    alarm_hbox = gtk_hbox_new(0, 0);
    gtk_container_add(GTK_CONTAINER(GTK_DIALOG(alarm_dialog)->vbox),
                      GTK_WIDGET(GTK_BOX(alarm_hbox)));

    dialog_button_box = gtk_hbox_new(1, 0);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(alarm_dialog)->vbox),
                       GTK_WIDGET(GTK_BOX(dialog_button_box)), 0, 0, 0);

    gtk_window_set_title(GTK_WINDOW(alarm_dialog),
                         dgettext("osso-clock", "cloc_ti_alarm_notification_title"));

    g_signal_connect_data(alarm_dialog, "delete-event", G_CALLBACK(gtk_true), 0, 0, 0);
    g_signal_connect_data(alarm_dialog, "response", G_CALLBACK(response_cb), 0, 0, 0);
    g_signal_connect_data(alarm_dialog, "key-press-event", G_CALLBACK(key_press_cb), 0, 0, 0);
    g_signal_connect_data(alarm_dialog, "key-release-event", G_CALLBACK(key_press_cb), 0, 0, 0);
  }

  gtk_widget_hide_all(GTK_WIDGET(dialog_button_box));
  gtk_container_foreach(GTK_CONTAINER(dialog_button_box), widget_destroy, 0);

  gtk_widget_hide_all(GTK_WIDGET(alarm_hbox));
  gtk_container_foreach(GTK_CONTAINER(alarm_hbox), widget_destroy, 0);

  g_slist_free(a->buttons);

  for(i=0; i < alarm_event->action_cnt; i++)
  {
    if ( alarm_action_is_button(&alarm_event->action_tab[i]) )
    {
      const char *textdomain = alarm_event_get_attr_string(alarm_event, "textdomain", NULL);
      GtkWidget * button;

      if ( textdomain )
        button = gtk_button_new_with_label(dgettext(textdomain, alarm_event->action_tab[i].label));
      else
        button = gtk_button_new_with_label(alarm_event->action_tab[i].label);

      gtk_container_add(GTK_CONTAINER(dialog_button_box), button);

      hildon_gtk_widget_set_theme_size(GTK_WIDGET(button), HILDON_SIZE_THUMB_HEIGHT);

      if ( alarm_event->action_tab[i].flags & ALARM_ACTION_TYPE_SNOOZE )
        a->snooze_action_index = i;
      a->buttons = g_slist_append(a->buttons,button);
      g_signal_connect_data(GTK_WIDGET(button), "clicked", G_CALLBACK(clicked_cb), (gpointer)a->cookie, NULL, 0);
    }
  }

  if ( !is_calendar(a) )
  {
    struct tm t;
    GtkWidget * alarm_message_label = NULL;
    gchar * s;
    GtkWidget * align;

    memset(&t, 0, sizeof(t));
    gtk_window_set_title(GTK_WINDOW(alarm_dialog),
                         dgettext("osso-clock", "cloc_ti_alarm_notification_title"));

    if ( alarm_event->message && *alarm_event->message )
    {
      alarm_message_label = gtk_label_new(0);
      gtk_label_set_line_wrap(GTK_LABEL(alarm_message_label), TRUE);
      gtk_label_set_text(GTK_LABEL(alarm_message_label), alarm_event->message);
      g_signal_connect_data(GTK_WIDGET(alarm_message_label), "size-request", G_CALLBACK(size_request_cb), 0, 0, 0);
    }

    time_get_synced();
    time_get_local_ex(alarm_event->trigger, &t);
    s = time2str(&t);
    time_label = gtk_label_new(s);
    g_free(s);

    hildon_helper_set_logical_font(time_label, "XXX-LargeSystemFont");
    hildon_helper_set_logical_color(time_label, GTK_RC_FG, 0, "DefaultTextColor");

    if ( alarm_event->message && *alarm_event->message )
    {
      hildon_helper_set_logical_font(alarm_message_label, "SystemFont");
      hildon_helper_set_logical_color(alarm_message_label, GTK_RC_FG, 0, "DefaultTextColor");
    }

    alarm_vbox = gtk_vbox_new(0, 0);
    align = gtk_alignment_new(0.5, 0.5, 0.0, 0.0);

    gtk_container_add(GTK_CONTAINER(align), GTK_WIDGET(alarm_vbox));
    gtk_container_add(GTK_CONTAINER(alarm_hbox), align);
    gtk_misc_set_alignment(GTK_MISC(alarm_vbox), 0.5, 0.5);
    gtk_box_pack_start(GTK_BOX(alarm_vbox), time_label, 0, 0, 0);

    if ( alarm_event->message && *alarm_event->message )
      gtk_box_pack_start(GTK_BOX(alarm_vbox), alarm_message_label, 1, 1, 0);

LABEL_20:
    alarm_notify(a->cookie);
    gtk_widget_show_all(GTK_WIDGET(alarm_dialog));
    return WindowPriority_ShowWindow(alarm_dialog, window_priority);
  }
  else
  {
    struct tm t;
    GtkWidget *hbox;
    GtkWidget *hbox1;
    GtkWidget *vbox;
    GtkWidget *align;
    GtkWidget *label;
    GtkWidget *location_label;
    GtkIconTheme *icon_theme;
    GdkPixbuf* pixbuf;

    memset(&t, 0, sizeof(t));

    hbox = gtk_hbox_new(0, 0);
    icon_theme = gtk_icon_theme_get_default();

    pixbuf = gtk_icon_theme_load_icon(icon_theme , "clock_calendar_alarm", 128, GTK_ICON_LOOKUP_NO_SVG, 0);

    if ( pixbuf )
    {
      GtkWidget *image = gtk_image_new_from_pixbuf(pixbuf);
      gtk_misc_set_alignment(GTK_MISC(image), 1.0, 0.5);
      gtk_box_pack_start(GTK_BOX(hbox), image, 0, 0, 4);
      gtk_widget_show(image);
      g_object_unref(pixbuf);
    }

    vbox = gtk_vbox_new(0, 4);
    align = gtk_alignment_new(0.0, 0.5, 1.0, 0.0);
    gtk_container_add(GTK_CONTAINER(align), vbox);

    hbox1 = gtk_hbox_new(0, 4);
    gtk_box_pack_end(GTK_BOX(hbox), align, 1, 1, 4);

    label = gtk_label_new(alarm_event->title?alarm_event->title:"");
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);

    hildon_helper_set_logical_font(label, "X-LargeSystemFont");
    hildon_helper_set_logical_color(label, GTK_RC_FG, 0, "DefaultTextColor");

    gtk_label_set_line_wrap(GTK_LABEL(label), 1);
    gtk_widget_set_size_request(label, 664, -1);
    g_signal_connect_data(label, "size-request", G_CALLBACK(size_request_cb), 0, 0, 0);
    gtk_box_pack_start(GTK_BOX(vbox), label, 1, 1, 0);

    gtk_label_set_text(GTK_LABEL(label), alarm_event->title?alarm_event->title:"");
    label = gtk_label_new(" ");
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);

    location_label = gtk_label_new(alarm_event_get_attr_string(alarm_event, "location", NULL));
    gtk_label_set_ellipsize(GTK_LABEL(location_label), PANGO_ELLIPSIZE_END);
    gtk_misc_set_alignment(GTK_MISC(location_label), 0.0, 0.5);
    hildon_helper_set_logical_font(location_label, "SystemFont");
    hildon_helper_set_logical_color(location_label, GTK_RC_FG, 0, "SecondaryTextColor");

    ptr = strdup(alarm_event_get_attr_string(alarm_event,
                                      ALARM_CALENDAR_EVENT_TYPE,
                                      "unset"));

    if ( strcmp(ptr, ALARM_FOR_NORMAL_EVENT) )
    {
      if ( strcmp(ptr, ALARM_FOR_ALLDAY) )
      {
        GdkPixbuf *icon = NULL;
        if ( strcmp(ptr, ALARM_FOR_TASK) )
        {
          if ( strcmp(ptr, ALARM_FOR_BIRTHDAY) )
            goto LABEL_34;
          set_label_time_text(label,alarm_event);
          gtk_box_pack_start(GTK_BOX(hbox1), label, 1, 1, 0);
          icon = gtk_icon_theme_load_icon(icon_theme, "calendar_birthday", 48, 1, 0);
        }
        else
        {
          set_label_time_text(label,alarm_event);
          gtk_box_pack_start(GTK_BOX(hbox1), label, 0, 0, 0);
          icon = gtk_icon_theme_load_icon(icon_theme, "calendar_todo", 48, 1, 0);
        }

        if ( icon )
        {
          GtkWidget *image = gtk_image_new_from_pixbuf(icon);
          gtk_misc_set_alignment(GTK_MISC(image), 0.0, 0.5);
          gtk_box_pack_end(GTK_BOX(hbox1), image, 0, 0, 4u);
          g_object_unref(icon);
        }
        goto LABEL_34;
      }
      set_label_time_text(label,alarm_event);
    }
    else
    {
      gchar * s;
      time_t tick = get_alarm_time(alarm_event);

      if ( tick )
      {
        struct tm lt,t;

        time_get_synced();
        time_get_local_ex(tick, &lt);
        memset(&t, 0, sizeof(t));
        time_get_local(&t);

        if ( tick - mktime(&t) <= 10800 )
        {
          s = time2str(&lt);
        }
        else
        {
          gchar *date = get_full_date(&lt);
          gchar *time = time2str(&lt);
          s = g_strconcat(date, " - ", time, NULL);
          g_free(date);
          g_free(time);
        }
        gtk_label_set_text(GTK_LABEL(label), s);
        hildon_helper_set_logical_font(label, "LargeSystemFont");
        g_free(s);
      }
    }

    gtk_box_pack_end(GTK_BOX(hbox1), label, 1, 1, 0);
  LABEL_34:
    if ( ptr )
      free(ptr);

    gtk_box_pack_start(GTK_BOX(vbox), hbox1, 0, 0, 0);
    gtk_box_pack_start(GTK_BOX(vbox), location_label, 1, 1, 0);
    gtk_box_pack_start(GTK_BOX(alarm_hbox), hbox, 1, 1, 0);
    goto LABEL_20;
  }

  return TRUE;
}

static gboolean  close_dialog(gpointer userdata)
{
  gtk_dialog_response((GtkDialog*)userdata, GTK_RESPONSE_NONE);
  return FALSE;
}

static void show_all_alarms()
{
  GSList *i;
  struct alarm * found = NULL;
  gboolean clock_alarm_found = FALSE;

  remove_unneeded_alarms(act_dead);

  for( i = alarms; i ; i = i->next)
  {
    struct alarm * a = (struct alarm *)i->data;

    if( a->pending || get_alarm_event_application(a) == 1 )
    {
      clock_alarm_found = TRUE;
      found = a;
    }
    else if(!clock_alarm_found)
      found = a;

    if(!i->next && found)
    {
      if ( get_alarm_event(found) )
      {
        show_alarm_dialog(found);
        return;
      }

      remove_alarm(found->cookie);
      i = alarms;
      found = NULL;
    }
  }

  if ( alarm_dialog )
  {
    WindowPriority_HideWindow(alarm_dialog);
    stop_timeouts(NULL);
    accelerometer_disable(-1);
    gtk_widget_destroy(GTK_WIDGET(alarm_dialog));
    alarm_dialog = 0;
    dbus_send_alarm_dialog_status(7);
  }

  if ( act_dead && alarm_events_cnt )
  {
    if ( alarm_events_cnt != 1 ||
         g_slist_length(alarms) == 0 ||
         ((struct alarm *)g_slist_nth_data(alarms,0))->pending ||
         ((struct alarm *)g_slist_nth_data(alarms,0))->event_index != -1 )
    {
      int dlg_result;
      GtkWidget* note = hildon_note_new_confirmation(GTK_WINDOW(alarm_dialog),
                                          dgettext("osso-clock", "cloc_fi_power_up_note_description"));

      WindowPriority_ShowWindow(note, window_priority);
      g_timeout_add_seconds(15, close_dialog, note);
      dbus_send_alarm_dialog_status(5);

      dlg_result = gtk_dialog_run(GTK_DIALOG(note));
      if ( dlg_result == GTK_RESPONSE_OK || dlg_result == GTK_RESPONSE_ACCEPT )
        last_event_index = 0x80000000u;
      else
        last_event_index = 0;

      WindowPriority_HideWindow(note);
      gtk_widget_destroy(note);
    }
    else
    {
      last_event_index = 0;
    }

    dbus_send_alarm_dialog_status(7);
    remove_unneeded_alarms(0);
    show_all_alarms();
  }
}

static gboolean calendar_timeout(gpointer user_data)
{
  struct alarm *a;

  accelerometer_disable((cookie_t)user_data);
  dbus_send_alarm_dialog_status(6);

  a = find_alarm((cookie_t)user_data);

  if ( a && a->snooze_cnt > 2 && act_dead )
  {
    a->calendar_timeout_tag = 0;
    if ( get_alarm_event(a) )
    {
      a->pending = 0;
      a->event_index = -1;
    }

    show_all_alarms();
  }

  return FALSE;
}
