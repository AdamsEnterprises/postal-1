/* postal-http.c
 *
 * Copyright (C) 2012 Catch.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include "neo-logger-daily.h"
#include "postal-bson.h"
#include "postal-debug.h"
#include "postal-http.h"
#include "postal-service.h"
#include "url-router.h"

static SoupServer *gServer;
static NeoLogger  *gLogger;

GQuark
postal_json_error_quark (void)
{
   return g_quark_from_static_string("PostalJsonError");
}

static void
postal_http_log_message (SoupMessage       *message,
                         const gchar       *path,
                         SoupClientContext *client)
{
   const gchar *ip_addr;
   const gchar *version;
   GTimeVal event_time;
   struct tm tt;
   time_t t;
   gchar *epath;
   gchar *formatted = NULL;
   gchar *referrer;
   gchar *user_agent;
   gchar ftime[32];

   g_assert(SOUP_IS_MESSAGE(message));
   g_assert(client);

   g_get_current_time(&event_time);
   t = (time_t)event_time.tv_sec;
   localtime_r(&t, &tt);
   strftime(ftime, sizeof ftime, "%b %d %H:%M:%S", &tt);
   ftime[31] = '\0';

   switch (soup_message_get_http_version(message)) {
   case SOUP_HTTP_1_0:
      version = "HTTP/1.0";
      break;
   case SOUP_HTTP_1_1:
      version = "HTTP/1.1";
      break;
   default:
      g_assert_not_reached();
      version = NULL;
      break;
   }

   ip_addr = soup_client_context_get_host(client);
   referrer = (gchar *)soup_message_headers_get_one(message->request_headers, "Referrer");
   user_agent = (gchar *)soup_message_headers_get_one(message->request_headers, "User-Agent");

   epath = g_strescape(path, NULL);
   referrer = g_strescape(referrer ?: "", NULL);
   user_agent = g_strescape(user_agent ?: "", NULL);

   formatted = g_strdup_printf("%s %s \"%s %s %s\" %u %"G_GSIZE_FORMAT" \"%s\" \"%s\"\n",
                               ip_addr,
                               ftime,
                               message->method,
                               epath,
                               version,
                               message->status_code,
                               message->response_body->length,
                               referrer,
                               user_agent);

   neo_logger_log(gLogger, &event_time, NULL, NULL, 0, 0, 0, NULL, formatted);

   g_free(epath);
   g_free(referrer);
   g_free(user_agent);
   g_free(formatted);
}

static void
postal_http_router (SoupServer        *server,
                    SoupMessage       *message,
                    const gchar       *path,
                    GHashTable        *query,
                    SoupClientContext *client,
                    gpointer           user_data)
{
   UrlRouter *router = user_data;

   ENTRY;

   g_assert(SOUP_IS_SERVER(server));
   g_assert(SOUP_IS_MESSAGE(message));
   g_assert(path);
   g_assert(client);

   if (!url_router_route(router, server, message, path, query, client)) {
      soup_message_set_status(message, SOUP_STATUS_NOT_FOUND);
   }

   EXIT;
}

static guint
get_int_param (GHashTable  *hashtable,
               const gchar *name)
{
   const gchar *str;

   if (hashtable) {
      if ((str = g_hash_table_lookup(hashtable, name))) {
         return g_ascii_strtoll(str, NULL, 10);
      }
   }

   return 0;
}

static JsonNode *
postal_http_parse_body (SoupMessage  *message,
                        GError      **error)
{
   JsonParser *p;
   JsonNode *ret;

   g_assert(SOUP_IS_MESSAGE(message));

   p = json_parser_new();

   if (!json_parser_load_from_data(p,
                                   message->request_body->data,
                                   message->request_body->length,
                                   error)) {
      g_object_unref(p);
      return NULL;
   }

   if ((ret = json_parser_get_root(p))) {
      ret = json_node_copy(ret);
   }

   g_object_unref(p);

   if (!ret) {
      g_set_error(error,
                  postal_json_error_quark(),
                  0,
                  "Missing JSON payload.");
   }

   return ret;
}

static void
postal_http_reply_bson_list (SoupMessage *message,
                             guint        status,
                             GList       *list)
{
   JsonGenerator *g;
   JsonArray *ar;
   JsonNode *node;
   JsonNode *child;
   GList *iter;
   gchar *json_buf;
   gsize length;

   g_assert(SOUP_IS_MESSAGE(message));

   ar = json_array_new();
   node = json_node_new(JSON_NODE_ARRAY);

   for (iter = list; iter; iter = iter->next) {
      if ((child = postal_bson_to_json(iter->data))) {
         json_array_add_element(ar, child);
      }
   }

   json_node_set_array(node, ar);
   json_array_unref(ar);

   g = json_generator_new();
   json_generator_set_root(g, node);
   json_node_free(node);
   json_generator_set_indent(g, 2);
   json_generator_set_pretty(g, TRUE);
   json_buf = json_generator_to_data(g, &length);
   g_object_unref(g);

   soup_message_set_response(message,
                             "application/json",
                             SOUP_MEMORY_TAKE,
                             json_buf,
                             length);
   soup_message_set_status(message, status ?: SOUP_STATUS_OK);
   soup_server_unpause_message(gServer, message);
}

static void
postal_http_reply_bson (SoupMessage     *message,
                        guint            status,
                        const MongoBson *bson)
{
   JsonGenerator *g;
   JsonNode *node;
   gchar *json_buf;
   gsize length;

   g_assert(SOUP_IS_MESSAGE(message));

   g = json_generator_new();
   node = postal_bson_to_json(bson);
   json_generator_set_root(g, node);
   json_node_free(node);
   json_generator_set_indent(g, 2);
   json_generator_set_pretty(g, TRUE);
   json_buf = json_generator_to_data(g, &length);
   g_object_unref(g);

   soup_message_set_response(message,
                             "application/json",
                             SOUP_MEMORY_TAKE,
                             json_buf,
                             length);
   soup_message_set_status(message, status ?: SOUP_STATUS_OK);
   soup_server_unpause_message(gServer, message);
}

static guint
get_status_code (const GError *error)
{
   guint code = SOUP_STATUS_INTERNAL_SERVER_ERROR;

   if (error->domain == POSTAL_DEVICE_ERROR) {
      switch (error->code) {
      case POSTAL_DEVICE_ERROR_MISSING_USER:
      case POSTAL_DEVICE_ERROR_MISSING_ID:
      case POSTAL_DEVICE_ERROR_INVALID_ID:
      case POSTAL_DEVICE_ERROR_NOT_FOUND:
         code = SOUP_STATUS_NOT_FOUND;
         break;
      case POSTAL_DEVICE_ERROR_INVALID_JSON:
         code = SOUP_STATUS_BAD_REQUEST;
         break;
      default:
         break;
      }
   } else if (error->domain == postal_json_error_quark()) {
      code = SOUP_STATUS_BAD_REQUEST;
   }

   return code;
}

static void
postal_http_reply_error (SoupMessage  *message,
                         const GError *error)
{
   JsonGenerator *g;
   JsonObject *obj;
   JsonNode *node;
   gchar *json_buf;
   gsize length;

   g_assert(SOUP_IS_MESSAGE(message));
   g_assert(error);

   obj = json_object_new();
   json_object_set_string_member(obj, "message", error->message);
   json_object_set_string_member(obj, "domain", g_quark_to_string(error->domain));
   json_object_set_int_member(obj, "code", error->code);

   node = json_node_new(JSON_NODE_OBJECT);
   json_node_set_object(node, obj);
   json_object_unref(obj);

   g = json_generator_new();
   json_generator_set_indent(g, 2);
   json_generator_set_pretty(g, TRUE);
   json_generator_set_root(g, node);
   json_node_free(node);
   json_buf = json_generator_to_data(g, &length);
   g_object_unref(g);

   soup_message_set_response(message,
                             "application/json",
                             SOUP_MEMORY_TAKE,
                             json_buf,
                             length);
   soup_message_set_status(message, get_status_code(error));
   soup_server_unpause_message(gServer, message);
}

static void
postal_http_reply_device(SoupMessage  *message,
                         guint         status,
                         PostalDevice *device)
{
   JsonGenerator *g;
   JsonNode *node;
   GError *error = NULL;
   gchar *json_buf;
   gsize length;

   ENTRY;

   g_assert(SOUP_IS_MESSAGE(message));
   g_assert(POSTAL_IS_DEVICE(device));

   if (!(node = postal_device_save_to_json(device, &error))) {
      postal_http_reply_error(message, error);
      soup_server_unpause_message(gServer, message);
      g_error_free(error);
      EXIT;
   }

   g = json_generator_new();
   json_generator_set_indent(g, 2);
   json_generator_set_pretty(g, TRUE);
   json_generator_set_root(g, node);
   json_node_free(node);
   if ((json_buf = json_generator_to_data(g, &length))) {
      soup_message_set_response(message,
                                "application/json",
                                SOUP_MEMORY_TAKE,
                                json_buf,
                                length);
   }
   soup_message_set_status(message, status);
   soup_server_unpause_message(gServer, message);
   g_object_unref(g);

   EXIT;
}

static void
device_cb (GObject      *object,
           GAsyncResult *result,
           gpointer      user_data)
{
   PostalService *service = (PostalService *)object;
   SoupMessage *message = user_data;
   MongoBson *bson;
   GError *error = NULL;

   ENTRY;

   g_assert(POSTAL_IS_SERVICE(service));
   g_assert(SOUP_IS_MESSAGE(message));

   if (!(bson = postal_service_find_device_finish(service, result, &error))) {
      postal_http_reply_error(message, error);
      g_error_free(error);
      GOTO(failure);
   } else {
      postal_http_reply_bson(message, SOUP_STATUS_OK, bson);
      mongo_bson_unref(bson);
   }

failure:
   g_object_unref(message);

   EXIT;
}

static void
device_delete_cb (GObject      *object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
   PostalService *service = (PostalService *)object;
   SoupMessage *message = user_data;
   GError *error = NULL;

   ENTRY;

   g_assert(POSTAL_IS_SERVICE(service));
   g_assert(SOUP_IS_MESSAGE(message));

   if (!postal_service_remove_device_finish(service, result, &error)) {
      postal_http_reply_error(message, error);
      g_error_free(error);
      GOTO(failure);
   } else {
      soup_message_set_status(message, SOUP_STATUS_NO_CONTENT);
      soup_message_headers_append(message->response_headers,
                                  "Content-Type",
                                  "application/json");
      soup_server_unpause_message(gServer, message);
   }

failure:
   g_object_unref(message);

   EXIT;
}

static void
device_put_cb (GObject      *object,
               GAsyncResult *result,
               gpointer      user_data)
{
   PostalService *service = (PostalService *)object;
   PostalDevice *device;
   SoupMessage *message = user_data;
   GError *error;

   g_assert(POSTAL_IS_SERVICE(service));
   g_assert(SOUP_IS_MESSAGE(message));

   if (!postal_service_update_device_finish(service, result, &error)) {
      postal_http_reply_error(message, error);
      g_error_free(error);
      GOTO(failure);
   }

   device = POSTAL_DEVICE(g_object_get_data(G_OBJECT(message), "device"));
   postal_http_reply_device(message, SOUP_STATUS_OK, device);

failure:
   g_object_unref(message);
}

static void
device_handler (UrlRouter         *router,
                SoupServer        *server,
                SoupMessage       *message,
                const gchar       *path,
                GHashTable        *params,
                GHashTable        *query,
                SoupClientContext *client,
                gpointer           user_data)
{
   MongoObjectId *oid;
   PostalDevice *pdev;
   const gchar *user;
   const gchar *device;
   GError *error = NULL;
   JsonNode *node;

   ENTRY;

   g_assert(router);
   g_assert(SOUP_IS_SERVER(server));
   g_assert(SOUP_IS_MESSAGE(message));
   g_assert(path);
   g_assert(params);
   g_assert(g_hash_table_contains(params, "device"));
   g_assert(g_hash_table_contains(params, "user"));
   g_assert(client);

   device = g_hash_table_lookup(params, "device");
   user = g_hash_table_lookup(params, "user");

   if (message->method == SOUP_METHOD_GET) {
      postal_service_find_device(POSTAL_SERVICE_DEFAULT,
                                 user,
                                 device,
                                 NULL, /* TODO */
                                 device_cb,
                                 g_object_ref(message));
      soup_server_pause_message(server, message);
      EXIT;
   } else if (message->method == SOUP_METHOD_DELETE) {
      if (!(oid = mongo_object_id_new_from_string(device))) {
         soup_message_set_status(message, SOUP_STATUS_NOT_FOUND);
         soup_server_unpause_message(gServer, message);
         EXIT;
      }
      pdev = g_object_new(POSTAL_TYPE_DEVICE,
                          "user", user,
                          "id", oid,
                          NULL);
      mongo_object_id_free(oid);
      postal_service_remove_device(POSTAL_SERVICE_DEFAULT,
                                   pdev,
                                   NULL, /* TODO */
                                   device_delete_cb,
                                   g_object_ref(message));
      soup_server_pause_message(server, message);
      g_object_unref(pdev);
      EXIT;
   } else if (message->method == SOUP_METHOD_PUT) {
      if (!(oid = mongo_object_id_new_from_string(device))) {
         soup_message_set_status(message, SOUP_STATUS_NOT_FOUND);
         soup_server_unpause_message(gServer, message);
         EXIT;
      }
      if (!(node = postal_http_parse_body(message, &error))) {
         postal_http_reply_error(message, error);
         soup_server_unpause_message(server, message);
         mongo_object_id_free(oid);
         g_error_free(error);
         EXIT;
      }
      pdev = g_object_new(POSTAL_TYPE_DEVICE,
                          "user", user,
                          "id", oid,
                          NULL);
      mongo_object_id_free(oid);
      if (!postal_device_load_from_json(pdev, node, &error)) {
         postal_http_reply_error(message, error);
         soup_server_unpause_message(server, message);
         json_node_free(node);
         g_error_free(error);
         g_object_unref(pdev);
         EXIT;
      }
      json_node_free(node);
      g_object_set_data_full(G_OBJECT(message), "device",
                             g_object_ref(pdev),
                             g_object_unref);
      postal_service_update_device(POSTAL_SERVICE_DEFAULT,
                                   pdev,
                                   NULL, /* TODO */
                                   device_put_cb,
                                   g_object_ref(message));
      soup_server_pause_message(server, message);
      g_object_unref(pdev);
      EXIT;
   } else {
      soup_message_set_status(message, SOUP_STATUS_METHOD_NOT_ALLOWED);
      EXIT;
   }

   g_assert_not_reached();
}

static void
devices_get_cb (GObject      *object,
                GAsyncResult *result,
                gpointer      user_data)
{
   PostalService *service = (PostalService *)object;
   SoupMessage *message = user_data;
   GError *error = NULL;
   GList *list;

   ENTRY;

   g_assert(POSTAL_IS_SERVICE(service));

   list = postal_service_find_devices_finish(service, result, &error);
   if (!list && error) {
      postal_http_reply_error(message, error);
      g_error_free(error);
      GOTO(failure);
   } else {
      postal_http_reply_bson_list(message, SOUP_STATUS_OK, list);
   }

   g_list_foreach(list, (GFunc)mongo_bson_unref, NULL);
   g_list_free(list);

failure:
   g_object_unref(message);

   EXIT;
}

static void
devices_post_cb (GObject      *object,
                 GAsyncResult *result,
                 gpointer      user_data)
{
   PostalService *service = (PostalService *)object;
   PostalDevice *device;
   SoupMessage *message = user_data;
   GError *error = NULL;
   gchar *str;
   gchar *oid;

   ENTRY;

   g_assert(POSTAL_IS_SERVICE(service));
   g_assert(SOUP_IS_MESSAGE(message));

   if (!postal_service_add_device_finish(service, result, &error)) {
      postal_http_reply_error(message, error);
      g_error_free(error);
      GOTO(failure);
   }

   if ((device = POSTAL_DEVICE(g_object_get_data(G_OBJECT(message), "device")))) {
      /*
       * Set Location: header.
       */
      oid = mongo_object_id_to_string(postal_device_get_id(device));
      str = g_strdup_printf("/v1/users/%s/devices/%s",
                            postal_device_get_user(device),
                            oid);
      soup_message_headers_append(message->response_headers,
                                  "Location",
                                  str);
      g_free(oid);
      g_free(str);

      /*
       * Set response body and 201 Created.
       */
      postal_http_reply_device(message, SOUP_STATUS_CREATED, device);
   } else {
      g_assert_not_reached();
   }

failure:
   g_object_unref(message);

   EXIT;
}

static void
devices_handler (UrlRouter         *router,
                 SoupServer        *server,
                 SoupMessage       *message,
                 const gchar       *path,
                 GHashTable        *params,
                 GHashTable        *query,
                 SoupClientContext *client,
                 gpointer           user_data)
{
   PostalDevice *device;
   const gchar *user;
   JsonNode *node;
   GError *error = NULL;

   ENTRY;

   g_assert(router);
   g_assert(SOUP_IS_SERVER(server));
   g_assert(SOUP_IS_MESSAGE(message));
   g_assert(path);
   g_assert(params);
   g_assert(g_hash_table_contains(params, "user"));
   g_assert(client);

   user = g_hash_table_lookup(params, "user");
   soup_server_pause_message(server, message);

   if (message->method == SOUP_METHOD_GET) {
      postal_service_find_devices(POSTAL_SERVICE_DEFAULT,
                                  user,
                                  get_int_param(query, "offset"),
                                  get_int_param(query, "limit"),
                                  NULL, /* TODO */
                                  devices_get_cb,
                                  g_object_ref(message));
      EXIT;
   } else if (message->method == SOUP_METHOD_POST) {
      if (!(node = postal_http_parse_body(message, &error))) {
         postal_http_reply_error(message, error);
         soup_server_unpause_message(server, message);
         g_error_free(error);
         EXIT;
      }
      device = postal_device_new();
      if (!postal_device_load_from_json(device, node, &error)) {
         postal_http_reply_error(message, error);
         json_node_free(node);
         g_object_unref(device);
         soup_server_unpause_message(server, message);
         g_error_free(error);
         EXIT;
      }
      postal_device_set_user(device, g_hash_table_lookup(params, "user"));
      g_object_set_data_full(G_OBJECT(message), "device", device, g_object_unref);
      postal_service_add_device(POSTAL_SERVICE_DEFAULT,
                                device,
                                NULL,
                                devices_post_cb,
                                g_object_ref(message));
      EXIT;
   }

   soup_message_set_status(message, SOUP_STATUS_METHOD_NOT_ALLOWED);
   soup_server_unpause_message(server, message);

   EXIT;
}

static void
notify_post_handler_cb (GObject      *object,
                        GAsyncResult *result,
                        gpointer      user_data)
{
   PostalService *service = (PostalService *)object;
   SoupMessage *message = user_data;
   GError *error = NULL;

   ENTRY;

   g_return_if_fail(POSTAL_IS_SERVICE(service));
   g_return_if_fail(SOUP_IS_MESSAGE(message));

   if (!postal_service_notify_finish(service, result, &error)) {
      postal_http_reply_error(message, error);
      g_error_free(error);
      g_object_unref(message);
      EXIT;
   }

   soup_message_set_status(message, SOUP_STATUS_OK);
   soup_server_unpause_message(gServer, message);
   soup_message_headers_append(message->response_headers,
                               "Content-Type",
                               "application/json");
   g_object_unref(message);

   EXIT;
}

static void
notify_post_handler (SoupServer        *server,
                     SoupMessage       *message,
                     const gchar       *path,
                     GHashTable        *query,
                     SoupClientContext *client,
                     gpointer           user_data)
{
   PostalNotification *notif;
   MongoObjectId *oid;
   const gchar *str;
   JsonObject *aps;
   JsonObject *c2dm;
   JsonObject *gcm;
   JsonObject *object;
   JsonArray *devices;
   JsonArray *users;
   GPtrArray *devices_ptr;
   GPtrArray *users_ptr;
   JsonNode *node;
   GError *error = NULL;
   guint count;
   guint i;

   g_assert(SOUP_IS_SERVER(server));
   g_assert(SOUP_IS_MESSAGE(message));
   g_assert(path);
   g_assert(client);

   if (!(node = postal_http_parse_body(message, &error))) {
      postal_http_reply_error(message, error);
      g_error_free(error);
      return;
   }

   if (!JSON_NODE_HOLDS_OBJECT(node) ||
       !(object = json_node_get_object(node)) ||
       !json_object_has_member(object, "aps") ||
       !(node = json_object_get_member(object, "aps")) ||
       !JSON_NODE_HOLDS_OBJECT(node) ||
       !(aps = json_object_get_object_member(object, "aps")) ||
       !json_object_has_member(object, "c2dm") ||
       !(node = json_object_get_member(object, "c2dm")) ||
       !JSON_NODE_HOLDS_OBJECT(node) ||
       !(c2dm = json_object_get_object_member(object, "c2dm")) ||
       !json_object_has_member(object, "gcm") ||
       !(node = json_object_get_member(object, "gcm")) ||
       !JSON_NODE_HOLDS_OBJECT(node) ||
       !(gcm = json_object_get_object_member(object, "gcm")) ||
       !json_object_has_member(object, "users") ||
       !(node = json_object_get_member(object, "users")) ||
       !JSON_NODE_HOLDS_ARRAY(node) ||
       !(users = json_object_get_array_member(object, "users")) ||
       !json_object_has_member(object, "devices") ||
       !(node = json_object_get_member(object, "devices")) ||
       !JSON_NODE_HOLDS_ARRAY(node) ||
       !(devices = json_object_get_array_member(object, "devices"))) {
      error = g_error_new(postal_json_error_quark(), 0,
                          "Missing or invalid fields in JSON payload.");
      postal_http_reply_error(message, error);
      json_node_free(node);
      g_error_free(error);
      return;
   }

   notif = g_object_new(POSTAL_TYPE_NOTIFICATION,
                        "aps", aps,
                        "c2dm", c2dm,
                        "gcm", gcm,
                        NULL);

   count = json_array_get_length(users);
   users_ptr = g_ptr_array_sized_new(count);
   for (i = 0; i < count; i++) {
      node = json_array_get_element(users, i);
      if (json_node_get_value_type(node) == G_TYPE_STRING) {
         str = json_node_get_string(node);
         g_ptr_array_add(users_ptr, (gchar *)str);
      }
   }
   g_ptr_array_add(users_ptr, NULL);

   count = json_array_get_length(devices);
   devices_ptr = g_ptr_array_sized_new(count);
   g_ptr_array_set_free_func(devices_ptr, (GDestroyNotify)mongo_object_id_free);
   for (i = 0; i < count; i++) {
      node = json_array_get_element(devices, i);
      if (json_node_get_value_type(node) == G_TYPE_STRING) {
         str = json_node_get_string(node);
         oid = mongo_object_id_new_from_string(str);
         g_ptr_array_add(devices_ptr, oid);
      }
   }

   postal_service_notify(POSTAL_SERVICE_DEFAULT,
                         notif,
                         (gchar **)users_ptr->pdata,
                         (MongoObjectId **)devices_ptr->pdata,
                         devices_ptr->len,
                         NULL, /* TODO: Cancellable/Timeout? */
                         notify_post_handler_cb,
                         g_object_ref(message));

   g_ptr_array_unref(devices_ptr);
   g_ptr_array_unref(users_ptr);
   json_node_free(node);
   g_object_unref(notif);
}

static void
notify_handler (UrlRouter         *router,
                SoupServer        *server,
                SoupMessage       *message,
                const gchar       *path,
                GHashTable        *params,
                GHashTable        *query,
                SoupClientContext *client,
                gpointer           user_data)
{
   g_assert(SOUP_IS_SERVER(server));
   g_assert(SOUP_IS_MESSAGE(message));
   g_assert(path);
   g_assert(client);

   if (message->method == SOUP_METHOD_POST) {
      soup_server_pause_message(server, message);
      notify_post_handler(server, message, path, params, client, user_data);
   } else {
      soup_message_set_status(message, SOUP_STATUS_METHOD_NOT_ALLOWED);
   }
}

static void
request_finished (SoupServer        *server,
                  SoupMessage       *message,
                  SoupClientContext *client,
                  gpointer           user_data)
{
   const gchar *path;
   SoupURI *uri;

   if (message && (uri = soup_message_get_uri(message))) {
      path = soup_uri_get_path(uri);
      postal_http_log_message(message, path, client);
   }
}

void
postal_http_init (GKeyFile *key_file)
{
   UrlRouter *router;
   gboolean nologging = FALSE;
   gchar *logfile = NULL;
   guint port = 0;

   if (key_file) {
      port = g_key_file_get_integer(key_file, "http", "port", NULL);
      logfile = g_key_file_get_string(key_file, "http", "logfile", NULL);
      nologging = g_key_file_get_boolean(key_file, "http", "nologging", NULL);
   }

   gServer = soup_server_new(SOUP_SERVER_PORT, port ?: 5300,
                             SOUP_SERVER_SERVER_HEADER, "Postal/"VERSION,
                             NULL);
   if (!nologging) {
      logfile = logfile ?: g_strdup("postal.log");
      gLogger = neo_logger_daily_new(logfile);
      g_signal_connect(gServer, "request-finished", G_CALLBACK(request_finished), NULL);
   }

   router = url_router_new();
   url_router_add_handler(router,
                          "/v1/users/:user/devices",
                          devices_handler, NULL);
   url_router_add_handler(router,
                          "/v1/users/:user/devices/:device",
                          device_handler, NULL);
   url_router_add_handler(router,
                          "/v1/notify",
                          notify_handler, NULL);

   soup_server_add_handler(gServer, NULL, postal_http_router,
                           router, (GDestroyNotify)url_router_free);
   soup_server_run_async(gServer);

   g_free(logfile);
}