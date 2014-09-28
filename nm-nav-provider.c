/*
* This file is part of the nokiamaps-navigation-provider.
*
* Copyright (C) 2014 Ivaylo Dimitrov <ivo.g.dimitrov.75@gmail.com>.
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License version
* 2 as published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful, but
* WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
* General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
* 02110-1301 USA
*
*/

#include <conic.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <gconf/gconf-client.h>
#include <gdk-pixbuf/gdk-pixbuf-loader.h>
#include <gdk-pixbuf/gdk-pixdata.h>
#include <glib.h>
#include <glib-object.h>
#include <glib/gthread.h>
#include <libxml/uri.h>
#include <libxml/nanohttp.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <location/location-distance-utils.h>
#include <navigation/navigation-provider.h>

#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#define NM_PROVIDER_TYPE (nm_provider_get_type ())

#define NM_PROVIDER_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE ((obj), \
            NM_PROVIDER_TYPE, NMProviderPrivate))

typedef struct _NMProvider NMProvider;
typedef struct _NMProviderClass NMProviderClass;
typedef struct _NMProviderPrivate NMProviderPrivate;
typedef struct _NMProviderCachedTile NMProviderCachedTile;
typedef struct _NMProviderThreadData NMProviderThreadData;
typedef struct _GetMapTileParams GetMapTileParams;
typedef struct _NMProviderLocation NMProviderLocation;
typedef struct _NMProviderExpiredLocation NMProviderExpiredLocation;

enum _NMProviderThreadFunc
{
  AddressToLocations,
  AddressToLocationsVerbose,
  FUNC0_2,
  LocationToAddress,
  LocationToAddressVerbose,
  GetMapTile,
  GetPOICategories
};

typedef enum _NMProviderThreadFunc NMProviderThreadFunc;

struct _NMProviderClass {
  GObjectClass parent_class;
};

struct _NMProvider {
  GObject parent;
  NMProviderPrivate *priv;
};

struct _NMProviderPrivate {
  const gchar *provider_url;
  DBusConnection *dbus;
  DBusGConnection *system_gdbus;
  GThreadPool *thread_pool;
  ConIcConnection *con_ic_conn;
  ConIcConnectionStatus con_ic_status;
  ConIcConnectionError con_ic_error;
  gboolean con_ic_do_not_connect;
  guint response_id;
  gchar *cache_dir;
  GSList *tile_list;
  GHashTable *loc_hash_table;
  int provider_twn;
};

struct _NMProviderCachedTile {
  gchar *filename;
  int timestamp;
};

struct _NMProviderThreadData
{
  NMProvider *provider;
  NMProviderThreadFunc func;
  gchar *responce;
  void *data;
};

struct _GetMapTileParams
{
  gdouble latitude;
  gdouble longitude;
  int zoom;
  int width;
  int height;
  int mapoptions;
};

struct _NMProviderLocation
{
  time_t timestamp;
  guint ref_cnt;
  NavigationAddress *navigation_data;
};

struct _NMProviderExpiredLocation
{
  NavigationLocation *location;
  time_t timestamp;
  guint ref_cnt;
};

G_LOCK_DEFINE_STATIC(hash_table);
G_LOCK_DEFINE_STATIC(conn_ic);

G_DEFINE_TYPE(NMProvider, nm_provider, G_TYPE_OBJECT);

static void nm_provider_class_finalize(GObject *object)
{
  ((GObjectClass*)nm_provider_parent_class)->finalize(object);
}

static void nm_provider_class_dispose(GObject *object)
{
  ((GObjectClass*)nm_provider_parent_class)->dispose(object);
}

static void nm_provider_init(NMProvider *provider)
{
  GConfClient *client;
  NMProviderPrivate *priv;

  /* FIXME - isn't provider_url supposed to be g_free()-ed in finalize? */
  client = gconf_client_get_default();
  priv = NM_PROVIDER_GET_PRIVATE(provider);
  provider->priv = priv;
  priv->provider_url =
      gconf_client_get_string(client,
                              "/apps/osso/navigation/nokiamaps_provider/url",
                              NULL);
  priv->provider_twn =
      gconf_client_get_bool(client,
                            "/apps/osso/navigation/nokiamaps_provider/twn",
                            NULL);
  if (!priv->provider_url)
  {
    gconf_client_set_string(client,
                            "/apps/osso/navigation/nokiamaps_provider/url",
                            "http://loc.desktop.maps.svc.ovi.com/geocoder",
                            NULL);
    priv->provider_url = "http://loc.desktop.maps.svc.ovi.com/geocoder";
  }

  g_object_unref(client);
}

static gboolean navigation_thread_pool_push(NMProviderThreadData *data)
{
  g_thread_pool_push(data->provider->priv->thread_pool, data, NULL);

  return FALSE;
}

static gboolean offline_mode(NMProviderPrivate *priv)
{
  DBusGProxy *proxy;
  gboolean rv;
  GError *error = NULL;
  char *device_mode;

  proxy = dbus_g_proxy_new_for_name(priv->system_gdbus,
                                    "com.nokia.mce",
                                    "/com/nokia/mce/request",
                                    "com.nokia.mce.request");
  dbus_g_proxy_call(proxy, "get_device_mode", &error,
                    G_TYPE_INVALID,
                    G_TYPE_STRING, &device_mode,
                    G_TYPE_INVALID);
  g_object_unref(proxy);

  if (error)
  {
    g_warning("%s: %s", __func__, error->message);
    g_error_free(error);
    return FALSE;
  }

  if (!g_strcmp0(device_mode, "flight") || !g_strcmp0(device_mode, "offline"))
    rv = TRUE;
  else
    rv = FALSE;
  g_free(device_mode);

  return rv;
}

static gboolean navigation_location_to_addresses(NMProvider *provider,
                                                 gdouble latitude,
                                                 gdouble longitude,
                                                 gboolean verbose,
                                                 gchar **objectpath,
                                                 GError **error)
{
  NMProviderThreadData *thread_data;
  NavigationLocation *location;

  if (!verbose && offline_mode(provider->priv))
  {
    g_set_error(error, g_quark_from_static_string("nm-navigation-provider"), 0,
                "%s not possible in offline mode", __func__);
    return FALSE;
  }

  location = (NavigationLocation *)g_malloc(sizeof(NavigationLocation));
  location->longitude = longitude;
  location->latitude = latitude;
  thread_data =
      (NMProviderThreadData *)g_malloc(sizeof(NMProviderThreadData));
  thread_data->provider = provider;
  thread_data->data = location;
  thread_data->func = (verbose ? LocationToAddressVerbose : LocationToAddress);
  thread_data->responce = g_strdup_printf("/nokiamaps/response/%u",
                                          provider->priv->response_id);
  provider->priv->response_id++;
  *objectpath = g_strdup(thread_data->responce);
  g_idle_add((GSourceFunc)navigation_thread_pool_push, thread_data);

  return TRUE;
}

static gboolean navigation_show_region(NMProvider *provider, gdouble nwlatitude,
                                       gdouble nwlongitude, gdouble selatitude,
                                       gdouble selongitude, guint mapoptions,
                                       GError **error)
{
  DBusMessage *message;

  message = dbus_message_new_method_call(
              "com.nokia.NokiaMaps",
              "/com/nokia/maps/NavigationProvider",
              "com.nokia.Navigation.MapProvider",
              "ShowRegion");
  if (!message)
  {
    g_set_error(error, g_quark_from_static_string("nm-navigation-provider"), 0,
                "Could not create new dbus method call");
    return FALSE;
  }

  dbus_message_append_args(
        message,
        DBUS_TYPE_DOUBLE, &nwlatitude,
        DBUS_TYPE_DOUBLE, &nwlongitude,
        DBUS_TYPE_DOUBLE, &selatitude,
        DBUS_TYPE_DOUBLE, &selongitude,
        DBUS_TYPE_UINT32, &mapoptions,
        DBUS_TYPE_INVALID);
  dbus_connection_send(provider->priv->dbus, message, NULL);
  dbus_message_unref(message);

  return TRUE;
}

static gboolean navigation_show_place_geo(NMProvider *provider,
                                          gdouble latitude, gdouble longitude,
                                          guint mapoptions, GError **error)
{
  DBusMessage *message;

  message = dbus_message_new_method_call(
              "com.nokia.NokiaMaps",
              "/com/nokia/maps/NavigationProvider",
              "com.nokia.Navigation.MapProvider",
              "ShowPlaceGeo");
  if (!message)
  {
    g_set_error(error, g_quark_from_static_string("nm-navigation-provider"), 0,
                "Could not create new dbus method call");
    return FALSE;
  }

  dbus_message_append_args(message,
                           DBUS_TYPE_DOUBLE, &latitude,
                           DBUS_TYPE_DOUBLE, &longitude,
                           DBUS_TYPE_UINT32, &mapoptions,
                           DBUS_TYPE_INVALID);
  dbus_connection_send(provider->priv->dbus, message, NULL);
  dbus_message_unref(message);

  return TRUE;
}

static gboolean navigation_show_places_topos(NMProvider *provider,
                                             const gchar **address,
                                             guint mapoptions, GError **error)
{
  DBusMessage *message;
  DBusMessageIter array;
  DBusMessageIter entry;

  message = dbus_message_new_method_call(
              "com.nokia.NokiaMaps",
              "/com/nokia/maps/NavigationProvider",
              "com.nokia.Navigation.MapProvider",
              "ShowPlacesTopos");
  if (!message)
  {
    g_set_error(error, g_quark_from_static_string("nm-navigation-provider"), 0,
                "Could not create new dbus method call");
    return FALSE;
  }

  dbus_message_iter_init_append(message, &array);
  dbus_message_iter_open_container(&array, DBUS_TYPE_ARRAY ,
                                   DBUS_TYPE_STRING_AS_STRING, &entry);
  while (*address)
  {
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, address);
    address ++;
  }

  dbus_message_iter_close_container(&array, &entry);
  dbus_message_iter_append_basic(&array, DBUS_TYPE_UINT32, &mapoptions);
  dbus_connection_send(provider->priv->dbus, message, NULL);
  dbus_message_unref(message);

  return TRUE;
}

static gboolean navigation_get_location_from_map(NMProvider *provider,
                                                 guint mapoption,
                                                 gchar **objectpath,
                                                 GError **error)
{
  DBusMessage *message;
  DBusMessage *reply;
  const char *path;
  gboolean rv = FALSE;

  message = dbus_message_new_method_call(
              "com.nokia.NokiaMaps",
              "/com/nokia/maps/NavigationProvider",
              "com.nokia.Navigation.MapProvider",
              "GetLocationFromMap");
  if (!message)
  {
    g_set_error(error, g_quark_from_static_string("nm-navigation-provider"), 0,
                "Could not create new dbus method call");
    goto out;
  }

  dbus_message_append_args(message,
                           DBUS_TYPE_UINT32, &mapoption,
                           DBUS_TYPE_INVALID);
  reply = dbus_connection_send_with_reply_and_block(provider->priv->dbus,
                                                    message, -1, NULL);
  dbus_message_unref(message);

  if (!reply)
  {
    g_set_error(error, g_quark_from_static_string("nm-navigation-provider"), 0,
                "Navigation provider could not connect to map application");
    goto out;
  }

  if (dbus_message_get_args(reply, NULL,
                             DBUS_TYPE_OBJECT_PATH, &path,
                             DBUS_TYPE_INVALID))
  {
    *objectpath = g_strdup(path);
    rv = TRUE;
  }
  else
    g_set_error(error, g_quark_from_static_string("nm-navigation-provider"), 0,
                "Could not parse object path from response");

  dbus_message_unref(reply);

out:
  return rv;
}

static gboolean navigation_show_route(NMProvider *provider,
                                      gdouble fromlatitude,
                                      gdouble fromlongitude,
                                      gdouble tolatitude,
                                      gdouble tolongitude,
                                      guint routeoptions,
                                      guint mapoptions,
                                      GError **error)
{
  DBusMessage *message;

  message = dbus_message_new_method_call(
              "com.nokia.NokiaMaps",
              "/com/nokia/maps/NavigationProvider",
              "com.nokia.Navigation.MapProvider",
              "ShowRoute");
  if (message)
  {
    g_set_error(error, g_quark_from_static_string("nm-navigation-provider"), 0,
                "Could not create new dbus method call");
    return FALSE;
  }

  dbus_message_append_args(
        message,
        DBUS_TYPE_DOUBLE, &fromlatitude,
        DBUS_TYPE_DOUBLE, &fromlongitude,
        DBUS_TYPE_DOUBLE, &tolatitude,
        DBUS_TYPE_DOUBLE, &tolongitude,
        DBUS_TYPE_UINT32, &routeoptions,
        DBUS_TYPE_UINT32, &mapoptions,
        DBUS_TYPE_INVALID);
  dbus_connection_send(provider->priv->dbus, message, NULL);
  dbus_message_unref(message);

  return TRUE;
}

static gboolean navigation_show_places_po_icategories(NMProvider *provider,
                                                      const char **categories,
                                                      guint mapoptions,
                                                      GError **error)
{
  DBusMessage *message;
  DBusMessageIter entry;
  DBusMessageIter array;

  message = dbus_message_new_method_call(
              "com.nokia.NokiaMaps",
              "/com/nokia/maps/NavigationProvider",
              "com.nokia.Navigation.MapProvider",
              "ShowPlacesPOICategories");
  if (!message)
  {
    g_set_error(error, g_quark_from_static_string("nm-navigation-provider"), 0,
                "Could not create new dbus method call");
    return FALSE;
  }

  dbus_message_iter_init_append(message, &array);
  dbus_message_iter_open_container(&array, DBUS_TYPE_ARRAY ,
                                   DBUS_TYPE_STRING_AS_STRING, &entry);
  while (*categories)
  {
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, categories);
    categories ++;
  }

  dbus_message_iter_close_container(&array, &entry);
  dbus_message_iter_append_basic(&array, DBUS_TYPE_UINT32, &mapoptions);
  dbus_connection_send(provider->priv->dbus, message, NULL);
  dbus_message_unref(message);

  return TRUE;
}

static gboolean navigation_get_po_icategories(NMProvider *provider,
                                              gchar **objectpath,
                                              GError **error G_GNUC_UNUSED)
{
  NMProviderPrivate *priv = provider->priv;
  NMProviderThreadData *thread_data;

  thread_data = (NMProviderThreadData *)g_malloc(sizeof(NMProviderThreadData));
  thread_data->func = GetPOICategories;
  thread_data->provider = provider;
  thread_data->data = NULL;
  thread_data->responce = g_strdup_printf("/nokiamaps/response/%u",
                                          priv->response_id);
  priv->response_id++;
  *objectpath = g_strdup(thread_data->responce);
  g_idle_add((GSourceFunc)navigation_thread_pool_push, thread_data);

  return TRUE;
}

static gboolean navigation_get_map_tile(NMProvider *provider,
                                        gdouble latitude,
                                        gdouble longitude,
                                        gint zoom,
                                        gint width,
                                        gint height,
                                        guint mapoptions,
                                        const char **objectpath,
                                        GError **error)
{
  GetMapTileParams *params;
  NMProviderThreadData *thread_data;
  NMProviderPrivate *priv;

  if (offline_mode(provider->priv))
  {
    g_set_error(error, g_quark_from_static_string("nm-navigation-provider"), 0,
                "%s not possible in offline mode", __func__);
    return FALSE;
  }

  params = (GetMapTileParams *)g_malloc(sizeof(GetMapTileParams));
  params->longitude = longitude;
  params->width = width;
  params->height = height;
  params->latitude = latitude;
  params->mapoptions = mapoptions;

  if (zoom > 18)
  {
    g_warning("Maximum zoom level is 18");
    params->zoom = 18;
  }
  else
    params->zoom = zoom;

  thread_data = (NMProviderThreadData *)g_malloc(sizeof(NMProviderThreadData));
  priv = provider->priv;
  thread_data->func = GetMapTile;
  thread_data->data = params;
  thread_data->provider = provider;
  thread_data->responce = g_strdup_printf("/nokiamaps/response/%u",
                                          priv->response_id);
  priv->response_id ++;
  *objectpath = g_strdup(thread_data->responce);
  g_idle_add((GSourceFunc)navigation_thread_pool_push, thread_data);

  return TRUE;
}

static gboolean navigation_address_to_locations(NMProvider *provider,
                                                const char **address,
                                                gboolean verbose,
                                                const char **objectpath,
                                                GError **error)
{
  NMProviderPrivate *priv = provider->priv;
  NMProviderThreadData *thread_data;
  GString *string;
  const char *names[5] = { "num", "str", "city", "zip", "ctr" };
  int index[5] = {0, 2, 4, 7, 8};
  int i;

  if (offline_mode(provider->priv)) {
    g_set_error(error, g_quark_from_static_string("nm-navigation-provider"), 0,
                "%s not possible in offline mode", __func__);
    return FALSE;
  }
  string = g_string_new(provider->priv->provider_url);
  g_string_append(string,
                  "/gc/1.0?total=1&token=9b87b24dffafdfcb6dfc66eeba834caa");

  /* FIXME - sizeof(index)/sizeof(index[0]) */
  for (i = 0; i < 5; i ++)
  {
    if (address[index[i]])
    {
      xmlChar *xmlstr = xmlURIEscapeStr((const xmlChar *)address[index[i]],
                                        NULL);
      gchar *str = g_strdup_printf("&%s=%s", names[i], xmlstr);
      g_string_append(string, str);
      g_free(str);
      xmlFree(xmlstr);
    }
  }

  thread_data = (NMProviderThreadData *)g_malloc(sizeof(NMProviderThreadData));
  thread_data->provider = provider;
  thread_data->data = g_string_free(string, 0);
  thread_data->func =
      (verbose ? AddressToLocationsVerbose : AddressToLocations);

  thread_data->responce = g_strdup_printf("/nokiamaps/response/%u",
                                          priv->response_id);
  priv->response_id ++;
  *objectpath = g_strdup(thread_data->responce);
  g_idle_add((GSourceFunc)navigation_thread_pool_push, thread_data);

  return TRUE;
}

static gboolean navigation_location_to_addresses_cached(
    NMProvider *provider,
    gdouble latitude,
    gdouble longitude,
    gdouble tolerance,
    GPtrArray **addresses,
    GError **error G_GNUC_UNUSED)
{
  NMProviderLocation *nearest;
  NavigationAddress *navigation_data = NULL;
  NavigationLocation location = { latitude, longitude };

  G_LOCK(hash_table);

  nearest =
      (NMProviderLocation *)g_hash_table_lookup(provider->priv->loc_hash_table,
                                                &location);
  if (!nearest && tolerance)
  {
    GHashTableIter hash_iter;
    gpointer tmp_key;
    gpointer tmp_val;
    gdouble best_distance = 0;

    tolerance /= 1000.0;

    g_hash_table_iter_init(&hash_iter, provider->priv->loc_hash_table);

    while (g_hash_table_iter_next(&hash_iter, &tmp_key, &tmp_val))
    {
      gdouble distance = location_distance_between(
            latitude, longitude,
            ((NavigationLocation *)tmp_key)->latitude,
            ((NavigationLocation *)tmp_key)->longitude);

      if (distance > tolerance)
        continue;

      if (best_distance == 0 || distance < best_distance)
      {
        best_distance = distance;
        nearest = tmp_val;
      }
    }
  }

  if (nearest)
  {
    navigation_data = navigation_address_copy(nearest->navigation_data);
    nearest->ref_cnt ++;
  }

  G_UNLOCK(hash_table);

  if (navigation_data)
  {
    *addresses = g_ptr_array_new();
    g_ptr_array_add(*addresses, address_to_array(navigation_data));
    navigation_address_free(navigation_data);

    return TRUE;
  }

  return FALSE;
}

#include "dbus_glib_marshal_navigation.h"

static void nm_provider_class_init(NMProviderClass *klass)
{
  klass->parent_class.finalize = nm_provider_class_finalize;
  klass->parent_class.dispose = nm_provider_class_dispose;

  dbus_g_object_type_install_info(klass->parent_class.g_type_class.g_type,
                                  &dbus_glib_navigation_object_info);
  g_type_class_add_private(klass, sizeof(NMProviderPrivate));
}

static gint compare_tiles(NMProviderCachedTile *a, NMProviderCachedTile *b)
{
  if (!a || !b)
    return 0;

  return b->timestamp - a->timestamp;
}

static guint location_hash(NavigationLocation *location)
{
  return 0x1F1F1F1F *
      (((guint)(location->latitude * 10000.0)) ^
       ((guint)(location->longitude * 10000.0)));
}

static gboolean location_equal(NavigationLocation *a, NavigationLocation *b)
{
  if (!a || !b)
    return FALSE;

  return (a->latitude == b->latitude) &&
      (a->longitude == b->longitude);
}

static void location_destroy_notify(NMProviderLocation *location)
{
  navigation_address_free(location->navigation_data);
  g_free(location);
}

static void navigation_address_to_locations_error_reply(DBusConnection *dbus,
                                                        const char *path,
                                                        const char *name)
{
  DBusMessage *msg;
  DBusMessageIter iter;
  const char *err_msg = "User canceled network connection opening";
  gushort value = 1;

  msg = dbus_message_new_signal(path, "com.nokia.Navigation.MapProvider", name);

  if (msg)
  {
    dbus_message_iter_init_append(msg, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT16, &value);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &err_msg);
    dbus_connection_send(dbus, msg, NULL);
    dbus_message_unref(msg);
  }
}

static xmlXPathObjectPtr get_path(xmlXPathContext *ctxt, const char *prefix,
                                  const char *ns_uri, const char *str)
{
  xmlXPathObjectPtr path;

  xmlXPathRegisterNs(ctxt, (const xmlChar *)prefix, (const xmlChar *)ns_uri);

  path = xmlXPathEval((const xmlChar *)str, ctxt);
  if (path)
  {
    if ( path->nodesetval && path->nodesetval->nodeNr )
    {
      if (!path->nodesetval->nodeTab)
      {
        xmlXPathFreeObject(path);
        path = NULL;
      }
    }
    else
    {
      xmlXPathFreeObject(path);
      path = NULL;
    }
  }

  return path;
}

static char *get_path_text(const char *path, xmlXPathContext *ctxt)
{
  xmlXPathObjectPtr obj;
  char *text = NULL;

  obj = xmlXPathEval((const xmlChar *)path, ctxt);
  if (obj)
  {
    if ( obj->nodesetval && obj->nodesetval->nodeNr && obj->nodesetval->nodeTab)
      text = (char *)xmlNodeGetContent(*obj->nodesetval->nodeTab);
    xmlXPathFreeObject(obj);
  }

  return text;
}

static xmlDocPtr http_request_reply(const char *url)
{
  xmlBufferPtr xml_buf;
  void *ctxt;
  xmlDoc *xml_doc;
  int len;
  char *content_type;
  xmlChar dest[1024];
  g_message(url);
  content_type = NULL;
  xml_buf = xmlBufferCreate();

#pragma message "OVI maps no longer supports \"Referer: Maemo_SW\", please find a replacement or remove that message"

  ctxt = xmlNanoHTTPMethod(url, "GET", NULL, &content_type,
#if 0
  /* FIXME - that breaks account status location, why? */
                           "Referer: Maemo_SW\n",
#else
                           NULL ,
#endif
                           0);
  if (ctxt && xmlNanoHTTPReturnCode(ctxt) == 200)
  {
    while (1)
    {
      len = xmlNanoHTTPRead(ctxt, dest, sizeof(dest));
      if (len <= 0)
        break;

      xmlBufferAdd(xml_buf, dest, len);
    }
  }

  xmlNanoHTTPClose(ctxt);
  g_free(content_type);
  xml_doc = xmlReadMemory((const char *)xml_buf->content, xml_buf->size, url,
                          NULL, 0);
  xmlBufferFree(xml_buf);

  return xml_doc;
}

static gboolean can_go_online(NMProviderPrivate *priv, gboolean verbose)
{
  /* FIXME simplify and reorder the condition bellow */
  /* FIXME - isn't there a race condition? i.e. like changing status/error in
     between g_atomic calls? Maybe a mutex is better/correct thing to use
  */

  return (!verbose ||
      g_atomic_int_get(&priv->con_ic_status) != CON_IC_STATUS_DISCONNECTED ||
      (g_atomic_int_get(&priv->con_ic_error) != CON_IC_CONNECTION_ERROR_USER_CANCELED &&
      g_atomic_int_get(&priv->con_ic_error)));
}

static void navigation_address_to_locations_reply(NMProviderThreadData *data,
                                                  gboolean verbose)
{
  DBusMessage *message;

  if (can_go_online(data->provider->priv, verbose)) {
    message = dbus_message_new_signal(data->responce,
                                      "com.nokia.Navigation.MapProvider",
                                      "AddressToLocationsReply");
    if (message)
    {
      DBusMessageIter entry;
      DBusMessageIter array;
      xmlDoc *xml_doc;

      dbus_message_iter_init_append(message, &array);
      dbus_message_iter_open_container(&array, DBUS_TYPE_ARRAY, "(dd)", &entry);

      xml_doc = http_request_reply((const char *)data->data);
      if (xml_doc)
      {
        xmlXPathContext *ctxt = xmlXPathNewContext(xml_doc);

        if (ctxt)
        {
          xmlXPathObject *path = get_path(
                ctxt, "gc",
                "nokia:geocoder:gc:1.0", "/gc:places/gc:place/gc:location");
          if (!path)
            path = get_path(
                  ctxt, "gc",
                  "nokia:search:gc:1.0", "/gc:response/gc:place/gc:location");
          if (path)
          {
            char *s;
            DBusMessageIter loc;
            NavigationLocation *location =
                (NavigationLocation *)g_malloc0(sizeof(NavigationLocation));

            s = get_path_text("//gc:position/gc:latitude", ctxt);
            location->latitude = g_ascii_strtod(s, NULL);
            g_free(s);
            s = get_path_text("//gc:position/gc:longitude", ctxt);
            location->longitude = g_ascii_strtod(s, NULL);
            g_free(s);
            dbus_message_iter_open_container(&entry, DBUS_TYPE_STRUCT, NULL,
                                             &loc);
            dbus_message_iter_append_basic(&loc,
                                           DBUS_TYPE_DOUBLE, location);
            dbus_message_iter_append_basic(&loc,
                                           DBUS_TYPE_DOUBLE,
                                           &location->longitude);
            dbus_message_iter_close_container(&entry, &loc);
            g_free(location);
            xmlXPathFreeObject(path);
            xmlXPathFreeContext(ctxt);
            xmlFreeDoc(xml_doc);
          }
          else
          {
            g_warning("Could not parse response");
            xmlXPathFreeContext(ctxt);
            xmlFreeDoc(xml_doc);
          }
        }
        else
        {
          g_warning("Could not create xpath context");
          xmlFreeDoc(xml_doc);
        }
      }
      else
        g_warning("Could not connect to %s", (const char *)data->data);

      dbus_message_iter_close_container(&array, &entry);
      dbus_connection_send(data->provider->priv->dbus, message, 0);
      dbus_message_unref(message);
    }
  }
  else
  {
    navigation_address_to_locations_error_reply(data->provider->priv->dbus,
                                                data->responce,
                                                "AddressToLocationError");
  }
}

static int expired_location_compare(NMProviderExpiredLocation *a,
                                    NMProviderExpiredLocation *b)
{
  if (!a || !b)
    return 0;

  if (a->ref_cnt == b->ref_cnt)
    return b->timestamp - a->timestamp;

  return b->ref_cnt - a->ref_cnt;
}

/* FIXME - looks ugly, ain't :) */
static void remove_expired(NMProviderPrivate *priv)
{
  GHashTable *hash_table;

  hash_table = priv->loc_hash_table;

  if (g_hash_table_size(hash_table) > 120)
  {
    GHashTableIter iter;
    time_t timer;
    GSList *list = NULL;
    GSList *nth;
    GSList *next;
    NMProviderExpiredLocation *data;

    time(&timer);
    timer -= 30 * 24 * 60 * 60;

    G_LOCK(hash_table);
    g_hash_table_iter_init(&iter, priv->loc_hash_table);

    while (1)
    {
      gpointer location;
      gpointer value;

      if (!g_hash_table_iter_next(&iter, &location, &value))
        break;

      while (((NMProviderLocation *)value)->timestamp < timer)
      {
        g_hash_table_iter_remove(&iter);

        if (!g_hash_table_iter_next(&iter, (gpointer *)&location,
                                    (gpointer *)&value))
          goto out;
      }

      data = (NMProviderExpiredLocation *)
          g_malloc0(sizeof(NMProviderExpiredLocation));
      data->location = location;
      data->timestamp = ((NMProviderLocation *)value)->timestamp;
      data->ref_cnt = ((NMProviderLocation *)value)->ref_cnt;

      list = g_slist_insert_sorted(list, data,
                                   (GCompareFunc)expired_location_compare);
    }

out:
    nth = g_slist_nth(list, 79);
    while (nth)
    {
      next = nth->next;
      if (!next)
        break;

      g_hash_table_remove(priv->loc_hash_table,
                          ((NMProviderExpiredLocation *)next->data)->location);
      nth = g_slist_delete_link(nth, nth->next);

      g_free(next->data);

    }

    G_UNLOCK(hash_table);
    g_slist_foreach(list, (GFunc)g_free, 0);
    g_slist_free(list);
  }
}

static void con_ic_status_handler(ConIcConnection *conn G_GNUC_UNUSED,
                                  ConIcConnectionEvent *event,
                                  NMProviderPrivate *priv)
{
  ConIcConnectionStatus status;

  status = con_ic_connection_event_get_status(event);

  if (status != (ConIcConnectionStatus)g_atomic_int_get(&priv->con_ic_status))
    g_atomic_int_set(&priv->con_ic_status, status);

  g_atomic_int_set(&priv->con_ic_error, con_ic_connection_event_get_error(event));

  G_UNLOCK(conn_ic);
}

/* FIXME
   Now, this is a total mess, not to say that stock code was causing a deadlock
   I fixed it to work, but it still uses linux-specific GStaticMutex
   implementation (according to Pali, it is futex that is on the bottom of this)
   This logic needs a complete re-write - maybe start another thread which does
   the con_ic_connection_connect call, somehow pass tid to callback(either as
   userdata or as a global variable), wait until that thread is finished
   (in con_ic_connect()) and kill that thread from con_ic_status_handler cb,
   releasing con_ic_connect()
 */
static void con_ic_connect(NMProviderPrivate *priv)
{
  G_LOCK(conn_ic);

  if (!priv->con_ic_conn)
  {
    priv->con_ic_conn = con_ic_connection_new();
    g_object_set(G_OBJECT(priv->con_ic_conn),
                 "automatic-connection-events", TRUE, NULL);

    g_signal_connect_data(G_OBJECT(priv->con_ic_conn), "connection-event",
                                   (GCallback)con_ic_status_handler,
                                   priv, NULL, 0);
    con_ic_connection_connect(priv->con_ic_conn, CON_IC_CONNECT_FLAG_NONE);
    G_LOCK(conn_ic);
  }
  else if (g_atomic_int_get(&priv->con_ic_status))
  {
    con_ic_connection_connect(priv->con_ic_conn, CON_IC_CONNECT_FLAG_NONE);
    G_LOCK(conn_ic);
  }

  G_UNLOCK(conn_ic);

  if (g_atomic_int_get(&priv->con_ic_do_not_connect))
    return;

  if (g_atomic_int_get(&priv->con_ic_status) == CON_IC_STATUS_DISCONNECTED )
  {
    if (g_atomic_int_get(&priv->con_ic_error) == CON_IC_CONNECTION_ERROR_USER_CANCELED ||
        g_atomic_int_get(&priv->con_ic_error) == CON_IC_CONNECTION_ERROR_NONE )
      g_atomic_int_set(&priv->con_ic_do_not_connect, TRUE);
  }
}

static dbus_bool_t iter_append_safe(DBusMessageIter *iter, char *value)
{
  const char *s = "";

  if (value)
    return dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &value);

  return dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &s);
}

static dbus_bool_t append_dbus_location_data(DBusMessageIter *array,
                                             NavigationAddress *address)
{
  DBusMessageIter elem;

  dbus_message_iter_open_container(array, DBUS_TYPE_ARRAY,
                                   DBUS_TYPE_STRING_AS_STRING, &elem);
  iter_append_safe(&elem, address->house_num);
  iter_append_safe(&elem, address->house_name);
  iter_append_safe(&elem, address->street);
  iter_append_safe(&elem, address->suburb);
  iter_append_safe(&elem, address->town);
  iter_append_safe(&elem, address->municipality);
  iter_append_safe(&elem, address->province);
  iter_append_safe(&elem, address->postal_code);
  iter_append_safe(&elem, address->country);
  iter_append_safe(&elem, address->country_code);
  iter_append_safe(&elem, address->time_zone);

  return dbus_message_iter_close_container(array, &elem);
}

static void navigation_location_to_address_reply(
    NMProviderThreadData *thread_data,
    gboolean verbose)
{
  NavigationLocation *location;
  DBusMessage *message;
  NMProviderLocation *provider_location;
  NMProviderPrivate *priv;
  GHashTable *hash_table = thread_data->provider->priv->loc_hash_table;
  DBusMessageIter sub;
  DBusMessageIter iter;

  location = (NavigationLocation *)thread_data->data;
  message = dbus_message_new_signal(thread_data->responce,
                                    "com.nokia.Navigation.MapProvider",
                                    "LocationToAddressReply");
  if (!message)
    return;

  dbus_message_iter_init_append(message, &iter);
  dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "as", &sub);

  G_LOCK(hash_table);
  provider_location =
      (NMProviderLocation *)g_hash_table_lookup(hash_table, location);
  G_UNLOCK(hash_table);

  if (provider_location)
  {
    append_dbus_location_data(&sub, provider_location->navigation_data);
    dbus_message_iter_close_container(&iter, &sub);
    provider_location->ref_cnt ++;
  }
  else
  {
    if (!g_atomic_int_get(&thread_data->provider->priv->con_ic_do_not_connect))
    {
      char lon[G_ASCII_DTOSTR_BUF_SIZE];
      char lat[G_ASCII_DTOSTR_BUF_SIZE];
      gchar *http_req;
      xmlDoc *xml_doc;

      priv = thread_data->provider->priv;
      con_ic_connect(thread_data->provider->priv);
      g_ascii_dtostr(lat, sizeof(lat), location->latitude);
      g_ascii_dtostr(lon, sizeof(lon), location->longitude);
      http_req = g_strdup_printf(
            "%s/rgc/1.0?total=1&lat=%s&long=%s&token=%s",
            priv->provider_url,
            lat,
            lon,
            "9b87b24dffafdfcb6dfc66eeba834caa");

      xml_doc = http_request_reply(http_req);
      if (xml_doc)
      {
        xmlXPathContext *ctxt = xmlXPathNewContext(xml_doc);

        if (ctxt)
        {
          NavigationAddress *address = NULL;
          xmlXPathObject *path = get_path(ctxt,
                                          "gc",
                                          "nokia:geocoder:gc:1.0",
                                          "/gc:places/gc:place/gc:address");

          if (!path)
            path = get_path(ctxt,
                            "gc",
                            "nokia:search:gc:1.0",
                            "/gc:response/gc:place/gc:address");
          if (path)
          {
            address =
                (NavigationAddress *)g_malloc0(sizeof(NavigationAddress));
            address->country = get_path_text("//gc:country", ctxt);
            address->country_code = get_path_text("//gc:countryCode", ctxt);
            address->suburb = get_path_text("//gc:district", ctxt);
            address->town = get_path_text("//gc:city", ctxt);
            address->postal_code = get_path_text("//gc:postCode", ctxt);
            address->street = get_path_text("//gc:thoroughfare/gc:name", ctxt);
            address->house_num =
                get_path_text("//gc:thoroughfare/gc:number", ctxt);
            xmlXPathFreeObject(path);

            if (priv->provider_twn &&
                g_strrstr_len(address->country, 6, "TAIWAN"))
            {
              g_free(address->country);
              address->country = g_strdup("TAIWAN");
            }
          }
          else
            g_warning("Could not parse response");

          xmlXPathFreeContext(ctxt);
          xmlFreeDoc(xml_doc);
          g_free(http_req);

          if (address)
          {
            append_dbus_location_data(&sub, address);
            dbus_message_iter_close_container(&iter, &sub);
            provider_location =
                (NMProviderLocation *)g_malloc0(sizeof(NMProviderLocation));
            time(&provider_location->timestamp);
            provider_location->navigation_data = address;
            provider_location->ref_cnt = 1;

            G_LOCK(hash_table);
            g_hash_table_insert(hash_table,
                                g_memdup(location, sizeof(NavigationLocation)),
                                provider_location);
            G_UNLOCK(hash_table);

            goto send_reply;
          }
        }
        else
        {
          g_warning("Could not create xpath context");
          xmlFreeDoc(xml_doc);
        }
      }
      else
        g_warning("Could not connect to %s", http_req);

      g_free(http_req);
    }

    if (can_go_online(thread_data->provider->priv, verbose))
      dbus_message_iter_close_container(&iter, &sub);
    else
    {
      dbus_message_iter_close_container(&iter, &sub);
      dbus_message_unref(message);
      navigation_address_to_locations_error_reply(
            thread_data->provider->priv->dbus,
            thread_data->responce,
            "LocationToAddressError");
      return;
    }
  }

send_reply:
  dbus_connection_send(thread_data->provider->priv->dbus, message, 0);
  dbus_message_unref(message);
  return;

}

static GdkPixbuf *download_tile(NMProviderPrivate *priv, const char *url)
{
  GdkPixbuf *rv = NULL;
  void *http_reply;
  GdkPixbufLoader *loader;
  guchar buffer[4096];
  GError *error = NULL;
  gchar *input = NULL;

  if (g_atomic_int_get(&priv->con_ic_do_not_connect))
    return NULL;

  con_ic_connect(priv);
  xmlNanoHTTPInit();

  http_reply =
      xmlNanoHTTPMethod(url, "GET", 0, &input, "Referer: Maemo_SW\n", 0);
  if (!http_reply)
  {
fail:
    g_warning("Failed to download map tile: %s", url);
    g_free(input);
    xmlNanoHTTPClose(http_reply);
    xmlNanoHTTPCleanup();
    return NULL;
  }

  if (xmlNanoHTTPReturnCode(http_reply) != 200)
  {
    g_warning("HTTP return code: %d", xmlNanoHTTPReturnCode(http_reply));
    goto fail;
  }

  loader = gdk_pixbuf_loader_new();

  while ( 1 )
  {
    int len = xmlNanoHTTPRead(http_reply, buffer, sizeof(buffer));
    if ( len <= 0 )
      break;

    if (!gdk_pixbuf_loader_write(loader, buffer, len, &error))
    {
      g_warning("Error loading map tile: %s\n", error->message);
      /* g_error_free was missing, yet another possible leak :( */
      g_error_free(error);
      break;
    }
  }

  if (gdk_pixbuf_loader_close(loader, NULL))
    rv = (GdkPixbuf *)g_object_ref(gdk_pixbuf_loader_get_pixbuf(loader));

  g_free(input);
  xmlNanoHTTPClose(http_reply);
  xmlNanoHTTPCleanup();

  if (loader)
    g_object_unref(G_OBJECT(loader));

  return rv;
}

#define long2x(lon) ((lon + 180.0) / 360.0)
#define deg2rad(deg) deg * M_PI / 180

static double lat2y(double lat)
{
  lat = deg2rad(lat);
  double y = log(tan(lat) + (1/cos(lat)));

  return (M_PI- y) / (2 * M_PI);
}

double y2lat(double y, double n)
{
  return
      (atan(exp((2.0 * (1.0 - y / n) - 1.0) * M_PI)) - M_PI/4) * 360.0 / M_PI;
}

double x2long(double x, double n)
{
  return ((2 * x / n) - 1.0) * 180.0;
}

inline static int roundup256(int n)
{
 int r = n % 256;
 return r ? n + 256 - r : n;
}

static void add_tile_to_list(NMProviderPrivate *priv, gchar *filename)
{
  GSList *tile_list;
  NMProviderCachedTile *tile;
  time_t timer;

  tile_list = priv->tile_list;
  time(&timer);

  if (tile_list)
  {
    while (g_strcmp0(*(const char **)tile_list->data, filename))
    {
      tile_list = tile_list->next;
      if (!tile_list)
        break;
    }

    if (tile_list)
    {
      tile = (NMProviderCachedTile *)tile_list->data;
      tile->timestamp = timer;
      return;
    }
  }

  tile = (NMProviderCachedTile *)g_malloc(sizeof(NMProviderCachedTile));

  tile->filename = g_strdup(filename);
  tile->timestamp = timer;
  priv->tile_list =
      g_slist_insert_sorted(priv->tile_list, tile, (GCompareFunc)compare_tiles);
}

static void save_tile_to_cache(NMProviderPrivate *priv,
                               GdkPixbuf *pixbuf, gchar *filename)
{
  if (pixbuf)
  {
    if (gdk_pixbuf_save(pixbuf, filename, "png", NULL, NULL))
      add_tile_to_list(priv, filename);
    else
      g_warning("Saving tile to cache failed: %s\n", filename);
  }
}

static void navigation_thread_func(NMProviderThreadData *thread_data,
                                   NMProviderPrivate *priv)
{
  NMProviderThreadFunc func;

  func = thread_data->func;

  if (func == AddressToLocations || func == AddressToLocationsVerbose)
  {
    if (!g_atomic_int_get(&priv->con_ic_do_not_connect))
      con_ic_connect(priv);
  }

  switch (func)
  {
    case AddressToLocations:
      navigation_address_to_locations_reply(thread_data, 0);
      break;
    case AddressToLocationsVerbose:
      navigation_address_to_locations_reply(thread_data, 1);
      break;
    default:
      break;
    case LocationToAddress:
      navigation_location_to_address_reply(thread_data, 0);
      remove_expired(priv);
      break;
    case LocationToAddressVerbose:
      navigation_location_to_address_reply(thread_data, 1);
      remove_expired(priv);
      break;
    case GetMapTile: {
      GetMapTileParams* tile_params = (GetMapTileParams *)thread_data->data;
      GdkPixbuf *tmp_pixbuf;
      GdkPixbuf *pixbuf;
      DBusMessage *message;
      DBusMessageIter array;

      const double tilesize = 256.0;
      double size = pow(2, tile_params->zoom);
      double xia = (tile_params->width / 2) / tilesize;
      double yia = (tile_params->height / 2) / tilesize;
      double x = long2x(tile_params->longitude) * size;
      double y = lat2y(tile_params->latitude) * size;
      double xi, yi, xoff, yoff;
      int pixleft = ((x - xia) - (int)(x - xia)) * tilesize;
      int pixtop = ((y - yia) - (int)(y - yia)) * tilesize;
      int wtmp  = roundup256(pixleft + tile_params->width);
      int htmp  = roundup256(pixtop + tile_params->height);

      gchar *tile_type;
      gchar *name_suffix;

      switch (tile_params->mapoptions & 0x1C)
      {
        case 4:
          tile_type = g_strdup("normal");
          break;
        case 8:
        case 0xC:
          tile_type = g_strdup("satellite");
          break;
        case 0x10:
          tile_type = g_strdup("terrain");
          break;
        default:
          tile_params->mapoptions |= 4;
          tile_type = g_strdup("normal");
          break;
      }

      if ((tile_params->mapoptions & 3) == 2)
        name_suffix = g_strconcat(tile_type, ".night", NULL);
      else
      {
        name_suffix = g_strconcat(tile_type, ".day", NULL);
        tile_params->mapoptions |= 1;
      }

      g_free(tile_type);
      /*
        TODO:
        In the original code the pixmap was without alpha channel, which was not
        working for some tiles (0400000900000505.png for example). I choose the
        easiest way and enabled the alpha channel on the temp pixbuf, which is
        not the best solution.The correct one is to strip the alpha channel
        before saving the tile.
       */
      tmp_pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, wtmp, htmp);
      pixbuf = gdk_pixbuf_new_subpixbuf(tmp_pixbuf,
                                        pixleft,
                                        pixtop,
                                        tile_params->width,
                                        tile_params->height);

      for (xi = - xia, xoff = 0; xoff < wtmp; xi ++, xoff += tilesize)
      {
        if (!pixbuf)
          break;

        for (yi = - yia, yoff = 0; yoff < htmp; yi ++, yoff += tilesize)
        {
          time_t timer;
          struct stat st;
          GdkPixbuf *tile_pixbuf;
          int namex = x + xi;
          int namey = y + yi;
          gchar *tile_fname =
              g_strdup_printf("%s/%02d%06d%06d%02d.png",
                              priv->cache_dir,
                              tile_params->zoom,
                              namex,
                              namey,
                              tile_params->mapoptions);
          gchar *url =
              g_strdup_printf(
                "%s/%s/%d/%d/%d/%d/%s?token=%s",
                "http://maptile.maps.svc.ovi.com/maptiler/maptile/newest",
                name_suffix,
                tile_params->zoom,
                namex,
                namey,
                256,
                "png8",
                "9b87b24dffafdfcb6dfc66eeba834caa");

          time(&timer);

          if (!stat(tile_fname, &st) &&
               (st.st_mtim.tv_sec > timer - 30 * 24 * 60 * 60))
          {
            tile_pixbuf = gdk_pixbuf_new_from_file(tile_fname, NULL);

            if (!tile_pixbuf)
            {
              g_warning("Cached tile corrupted,reloading from server\n");
              tile_pixbuf = download_tile(priv, url);
              save_tile_to_cache(priv, tile_pixbuf, tile_fname);
            }
            else
              add_tile_to_list(priv, tile_fname);
          }
          else
          {
            tile_pixbuf = download_tile(priv, url);
            save_tile_to_cache(priv, tile_pixbuf, tile_fname);
          }

          if (tile_pixbuf)
          {
            gdk_pixbuf_scale(tile_pixbuf, tmp_pixbuf, xoff, yoff, tilesize,
                             tilesize, xoff, yoff, 1.0, 1.0,
                             GDK_INTERP_NEAREST);
            g_object_unref(tile_pixbuf);
            tile_pixbuf = NULL;
          }
          else
          {
            g_warning("Could not get map tile");
            g_object_unref(pixbuf);
            pixbuf = 0;
          }

          g_free(tile_fname);
          g_free(url);
          if (!pixbuf)
            break;
        }
      }

      g_free(name_suffix);
      g_object_unref(tmp_pixbuf);

      message = dbus_message_new_signal(thread_data->responce,
                                        "com.nokia.Navigation.MapProvider",
                                        "GetMapTileReply");
      if (message)
      {
        dbus_message_iter_init_append(message, &array);

        if(pixbuf)
        {
          double nwlat = y2lat(y - yia, size);
          double nwlong = x2long(x - xia, size);
          double selat = y2lat(y + yia, size);
          double selong = x2long(x + xia, size);
          DBusMessageIter elem;
          GdkPixdata pixdata;
          guint8 *pixdata_buffer;
          guint len;

          gdk_pixdata_from_pixbuf(&pixdata, pixbuf, FALSE);
          pixdata_buffer = gdk_pixdata_serialize(&pixdata, &len);

          dbus_message_iter_open_container(&array, DBUS_TYPE_ARRAY,
                                           DBUS_TYPE_BYTE_AS_STRING, &elem);
          dbus_message_iter_append_fixed_array(&elem, DBUS_TYPE_BYTE,
                                               &pixdata_buffer, len);
          dbus_message_iter_close_container(&array, &elem);

          dbus_message_iter_open_container(&array,
                                           DBUS_TYPE_STRUCT, NULL, &elem);
          dbus_message_iter_append_basic(&elem, DBUS_TYPE_DOUBLE, &nwlat);
          dbus_message_iter_append_basic(&elem, DBUS_TYPE_DOUBLE, &nwlong);
          dbus_message_iter_close_container(&array, &elem);

          dbus_message_iter_open_container(&array,
                                           DBUS_TYPE_STRUCT, NULL, &elem);
          dbus_message_iter_append_basic(&elem, DBUS_TYPE_DOUBLE, &selat);
          dbus_message_iter_append_basic(&elem, DBUS_TYPE_DOUBLE, &selong);
          dbus_message_iter_close_container(&array, &elem);

          dbus_connection_send(priv->dbus, message, NULL);

          dbus_message_unref(message);
          g_free(pixdata_buffer);
        }
        else
        {
          dbus_connection_send(priv->dbus, message, NULL);
          dbus_message_unref(message);
        }
      }

      if(pixbuf)
        g_object_unref(pixbuf);

      break;
    }
    case GetPOICategories:
    {
      DBusMessageIter array;
      DBusMessageIter elem;
      /* Don't blame me, it was like that :) */
      static const char *categories[4] =
      {
        "TODO_category1",
        "TODO_category2",
        "TODO_category3",
        NULL
      };
      const char **cat = categories;
      DBusMessage *message =
          dbus_message_new_signal(thread_data->responce,
                                  "com.nokia.Navigation.MapProvider",
                                  "GetPOICategoriesReply");
      if (message)
      {
        dbus_message_iter_init_append(message, &array);
        dbus_message_iter_open_container(&array, DBUS_TYPE_ARRAY,
                                         DBUS_TYPE_STRING_AS_STRING, &elem);

        while (*cat)
        {
          dbus_message_iter_append_basic(&elem, DBUS_TYPE_STRING, cat);
          cat ++;
        }

        dbus_message_iter_close_container(&array, &elem);
        dbus_connection_send(thread_data->provider->priv->dbus, message, NULL);
        dbus_message_unref(message);
      }

      break;
    }
  }

  if (g_atomic_int_get(&priv->con_ic_do_not_connect) &&
      !g_thread_pool_unprocessed(priv->thread_pool))
    g_atomic_int_set(&priv->con_ic_do_not_connect, FALSE);

  g_free(thread_data->data);
  g_free(thread_data->responce);
  g_free(thread_data);
}

int main()
{
  NMProvider *provider;
  NMProviderPrivate *priv;
  DBusGConnection *session_gdbus;
  DBusGProxy *proxy;
  GDir *dir;
  GMainLoop *loop;
  GError *error = NULL;
  guint request_name_result;

  g_thread_init(NULL);
  g_type_init();
  loop = g_main_loop_new(NULL, FALSE);

  session_gdbus = dbus_g_bus_get(DBUS_BUS_SESSION, &error);
  if (!session_gdbus)
  {
    g_error("Error getting bus: %s", error->message);
    while (1);
  }

  proxy = dbus_g_proxy_new_for_name(
            session_gdbus,
            "org.freedesktop.DBus",
            "/org/freedesktop/DBus",
            "org.freedesktop.DBus");
  if (!dbus_g_proxy_call(
        proxy, "RequestName", &error,
        G_TYPE_STRING, "com.nokia.Navigation.NokiaMapsProvider",
        G_TYPE_UINT, 0,
        G_TYPE_INVALID,
        G_TYPE_UINT, &request_name_result,
        G_TYPE_INVALID))
  {
    g_error("Error registering D-Bus service: %s", error->message);
    /* WHAT ?!? */
    while (1)
      ;
  }

  if (request_name_result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
  {
    g_critical("Error registering D-Bus: could not get primary ownership!");
    return 1;
  }

  provider = (NMProvider *)g_object_new(NM_PROVIDER_TYPE, NULL);
  g_atomic_int_set(&provider->priv->con_ic_status,
                   CON_IC_STATUS_DISCONNECTED);
  priv = provider->priv;
  priv->thread_pool = g_thread_pool_new((GFunc)navigation_thread_func, priv,
                                        1, FALSE, NULL);
  g_atomic_int_set(&priv->con_ic_do_not_connect, FALSE);
  priv->dbus = dbus_g_connection_get_connection(session_gdbus);
  priv->cache_dir = g_strdup_printf("%s/MyDocs/.map_tile_cache",
                                    (gchar*)g_get_home_dir());
  if ( !g_file_test(priv->cache_dir,
                    (GFileTest)G_FILE_TEST_EXISTS|G_FILE_TEST_IS_DIR)
       && g_mkdir_with_parents(provider->priv->cache_dir, 0770) )
    g_warning("Map tile cache directory does not exist and could not create it. Cache directory: %s",
              priv->cache_dir);

  dir = g_dir_open(priv->cache_dir, 0, NULL);
  if (dir)
  {
    for ( ; ; )
    {
      const gchar *fname = g_dir_read_name(dir);

      if (!fname)
        break;

      if (g_strstr_len(fname, -1, ".png"))
      {
        struct stat stat_buf;
        gchar *pngfname = g_strdup_printf("%s/%s", priv->cache_dir, fname);
        NMProviderCachedTile *tile =
            (NMProviderCachedTile *)g_malloc(sizeof(NMProviderCachedTile));

        tile->filename = pngfname;

        if (!stat(pngfname, &stat_buf))
        {
          tile->timestamp = stat_buf.st_mtim.tv_sec;
          priv->tile_list = g_slist_insert_sorted(priv->tile_list, tile,
                                                  (GCompareFunc)compare_tiles);
        }
      }
    }

    g_dir_close(dir);
  }
  else
    g_warning("Could not read files from cache");

  priv->loc_hash_table =
      g_hash_table_new_full((GHashFunc)location_hash,
                            (GEqualFunc)location_equal,
                            g_free,
                            (GDestroyNotify)location_destroy_notify);

  priv->system_gdbus = dbus_g_bus_get(DBUS_BUS_SYSTEM, &error);;
  if (!priv->system_gdbus)
  {
    g_error("Error getting system bus: %s", error->message);
    /* again - WHAT?!? */
    while ( 1 )
      ;
  }

  priv->response_id = 0;
  dbus_g_connection_register_g_object(session_gdbus, "/Provider",
                                      &provider->parent);
  g_main_loop_run(loop);
  g_main_loop_unref(loop);
  g_object_unref(provider);
  g_object_unref(proxy);

  return 0;
}
