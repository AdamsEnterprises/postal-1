#include <postal/postal-application.h>
#include <postal/postal-device.h>
#include <libsoup/soup.h>
#include <string.h>

static PostalApplication *gApplication;
static GKeyFile          *gKeyFile;
static gchar             *gAccount;
static gchar             *gC2dmDevice;
static gint               gC2dmDeviceId;
static const gchar       *gConfig =
   "[mongo]\n"
   "db = test\n"
   "collection = devices\n"
   "uri = mongodb://127.0.0.1:27017\n"
   "[http]\n"
   "port = 6616\n"
   "nologging = true\n";

static PostalApplication *
application_new (const gchar *name)
{
   PostalApplication *app;
   gchar *app_name;

   app_name = g_strdup_printf("com.catch.postald.http.%s", name);
   app = g_object_new(POSTAL_TYPE_APPLICATION,
                      "application-id", app_name,
                      "flags", G_APPLICATION_HANDLES_COMMAND_LINE | G_APPLICATION_NON_UNIQUE,
                      NULL);
   neo_application_set_config(NEO_APPLICATION(app), gKeyFile);
   g_free(app_name);

   return app;
}

static void
get_devices_cb (SoupSession *session,
                SoupMessage *message,
                gpointer     user_data)
{
   g_assert(SOUP_IS_SESSION(session));
   g_assert(SOUP_IS_MESSAGE(message));

   g_assert_cmpint(message->status_code, ==, 200);
   g_assert_cmpstr(message->response_body->data, ==, "[\n]");

   g_application_quit(G_APPLICATION(gApplication));
}

static void
test1 (void)
{
   SoupSession *session;
   SoupMessage *message;
   gchar *url;

   gApplication = application_new(G_STRFUNC);

   session = soup_session_async_new();
   g_assert(SOUP_IS_SESSION(session));

   url = g_strdup_printf("http://127.0.0.1:6616/v1/users/%s/devices", gAccount);
   message = soup_message_new("GET", url);
   g_assert(SOUP_IS_MESSAGE(message));
   g_free(url);

   soup_session_queue_message(session, message, get_devices_cb, NULL);

   g_application_run(G_APPLICATION(gApplication), 0, NULL);

   g_clear_object(&gApplication);
}

static void
add_device_cb (SoupSession *session,
               SoupMessage *message,
               gpointer     user_data)
{
   PostalDevice *device;
   JsonParser *parser;
   JsonNode *node;
   gboolean r;
   GError *error = NULL;
   gchar *str;

   g_assert(SOUP_IS_SESSION(session));
   g_assert(SOUP_IS_MESSAGE(message));

   g_assert_cmpint(message->status_code, ==, 201);

   parser = json_parser_new();
   r = json_parser_load_from_data(parser,
                                  message->response_body->data,
                                  message->response_body->length,
                                  &error);
   node = json_parser_get_root(parser);
   g_assert_no_error(error);
   g_assert(r);

   device = postal_device_new();
   r = postal_device_load_from_json(device, node, &error);
   g_assert_no_error(error);
   g_assert(r);

   g_object_unref(parser);

   str = g_strdup_printf("%064u", gC2dmDeviceId);
   g_assert_cmpstr(str, ==, postal_device_get_device_token(device));
   g_free(str);

   g_assert(postal_device_get_device_type(device) == POSTAL_DEVICE_C2DM);
   g_assert_cmpstr(postal_device_get_user(device), ==, gAccount);
   g_assert(!postal_device_get_removed_at(device));
   g_assert(postal_device_get_created_at(device));

   g_application_quit(G_APPLICATION(gApplication));
}

static void
test2 (void)
{
   SoupSession *session;
   SoupMessage *message;
   gchar *url;

   gApplication = application_new(G_STRFUNC);

   session = soup_session_async_new();
   g_assert(SOUP_IS_SESSION(session));

   url = g_strdup_printf("http://127.0.0.1:6616/v1/users/%s/devices/%064u",
                         gAccount, gC2dmDeviceId);
   message = soup_message_new("PUT", url);
   g_assert(SOUP_IS_MESSAGE(message));
   g_free(url);

   soup_message_headers_set_content_type(message->request_headers,
                                         "application/json",
                                         NULL);
   soup_message_body_append(message->request_body,
                            SOUP_MEMORY_STATIC,
                            gC2dmDevice,
                            strlen(gC2dmDevice));

   soup_session_queue_message(session, message, add_device_cb, NULL);

   g_application_run(G_APPLICATION(gApplication), 0, NULL);

   g_clear_object(&gApplication);
}

static void
get_devices2_cb (SoupSession *session,
                 SoupMessage *message,
                 gpointer     user_data)
{
   PostalDevice *device;
   JsonParser *parser;
   JsonNode *node;
   gboolean r;
   GError *error = NULL;
   GList *elements;
   GList *iter;
   gchar *str;

   g_assert(SOUP_IS_SESSION(session));
   g_assert(SOUP_IS_MESSAGE(message));

   g_assert_cmpint(message->status_code, ==, 200);

   parser = json_parser_new();
   r = json_parser_load_from_data(parser,
                                  message->response_body->data,
                                  message->response_body->length,
                                  &error);
   node = json_parser_get_root(parser);
   g_assert_no_error(error);
   g_assert(r);

   g_assert(JSON_NODE_HOLDS_ARRAY(node));

   elements = json_array_get_elements(json_node_get_array(node));
   g_assert_cmpint(1, ==, g_list_length(elements));

   for (iter = elements; iter; iter = iter->next) {
      device = postal_device_new();
      r = postal_device_load_from_json(device, iter->data, &error);
      g_assert_no_error(error);
      g_assert(r);

      str = g_strdup_printf("%064u", gC2dmDeviceId);
      g_assert_cmpstr(str, ==, postal_device_get_device_token(device));
      g_free(str);

      g_assert(postal_device_get_device_type(device) == POSTAL_DEVICE_C2DM);
      g_assert_cmpstr(postal_device_get_user(device), ==, gAccount);
      g_assert(!postal_device_get_removed_at(device));
      g_assert(postal_device_get_created_at(device));
   }

   g_object_unref(parser);

   g_application_quit(G_APPLICATION(gApplication));
}

static void
test3 (void)
{
   SoupSession *session;
   SoupMessage *message;
   gchar *url;

   gApplication = application_new(G_STRFUNC);

   session = soup_session_async_new();
   g_assert(SOUP_IS_SESSION(session));

   url = g_strdup_printf("http://127.0.0.1:6616/v1/users/%s/devices", gAccount);
   message = soup_message_new("GET", url);
   g_assert(SOUP_IS_MESSAGE(message));
   g_free(url);

   soup_session_queue_message(session, message, get_devices2_cb, NULL);

   g_application_run(G_APPLICATION(gApplication), 0, NULL);

   g_clear_object(&gApplication);
}

static void
get_device_cb (SoupSession *session,
               SoupMessage *message,
               gpointer     user_data)
{
   PostalDevice *device;
   JsonParser *parser;
   JsonNode *node;
   gboolean r;
   GError *error = NULL;
   gchar *str;

   g_assert(SOUP_IS_SESSION(session));
   g_assert(SOUP_IS_MESSAGE(message));

   g_assert_cmpint(message->status_code, ==, 200);

   parser = json_parser_new();
   r = json_parser_load_from_data(parser,
                                  message->response_body->data,
                                  message->response_body->length,
                                  &error);
   node = json_parser_get_root(parser);
   g_assert_no_error(error);
   g_assert(r);

   device = postal_device_new();
   r = postal_device_load_from_json(device, node, &error);
   g_assert_no_error(error);
   g_assert(r);

   str = g_strdup_printf("%064u", gC2dmDeviceId);
   g_assert_cmpstr(str, ==, postal_device_get_device_token(device));
   g_free(str);

   g_assert(postal_device_get_device_type(device) == POSTAL_DEVICE_C2DM);
   g_assert_cmpstr(postal_device_get_user(device), ==, gAccount);
   g_assert(!postal_device_get_removed_at(device));
   g_assert(postal_device_get_created_at(device));

   g_object_unref(parser);

   g_application_quit(G_APPLICATION(gApplication));
}

static void
test4 (void)
{
   SoupSession *session;
   SoupMessage *message;
   gchar *url;

   gApplication = application_new(G_STRFUNC);

   session = soup_session_async_new();
   g_assert(SOUP_IS_SESSION(session));

   url = g_strdup_printf("http://127.0.0.1:6616/v1/users/%s/devices/%064u", gAccount, gC2dmDeviceId);
   message = soup_message_new("GET", url);
   g_assert(SOUP_IS_MESSAGE(message));
   g_free(url);

   soup_session_queue_message(session, message, get_device_cb, NULL);

   g_application_run(G_APPLICATION(gApplication), 0, NULL);

   g_clear_object(&gApplication);
}

static void
put_cb (SoupSession *session,
        SoupMessage *message,
        gpointer     user_data)
{
   PostalDevice *device;
   JsonParser *parser;
   JsonNode *node;
   gboolean r;
   GError *error = NULL;
   gchar *str;

   g_assert(SOUP_IS_SESSION(session));
   g_assert(SOUP_IS_MESSAGE(message));

   g_assert_cmpint(message->status_code, ==, 200);

   parser = json_parser_new();
   r = json_parser_load_from_data(parser,
                                  message->response_body->data,
                                  message->response_body->length,
                                  &error);
   node = json_parser_get_root(parser);
   g_assert_no_error(error);
   g_assert(r);

   device = postal_device_new();
   r = postal_device_load_from_json(device, node, &error);
   g_assert_no_error(error);
   g_assert(r);

   g_object_unref(parser);

   str = g_strdup_printf("%064u", gC2dmDeviceId);
   g_assert_cmpstr(str, ==, postal_device_get_device_token(device));
   g_free(str);

   g_assert(postal_device_get_device_type(device) == POSTAL_DEVICE_GCM);
   g_assert_cmpstr(postal_device_get_user(device), ==, gAccount);
   g_assert(!postal_device_get_removed_at(device));
   g_assert(postal_device_get_created_at(device));

   g_object_unref(device);

   g_application_quit(G_APPLICATION(gApplication));
}

static void
test5 (void)
{
   SoupSession *session;
   SoupMessage *message;
   gchar *url;
   gchar *str;

   gApplication = application_new(G_STRFUNC);

   session = soup_session_async_new();
   g_assert(SOUP_IS_SESSION(session));

   url = g_strdup_printf("http://127.0.0.1:6616/v1/users/%s/devices/%064u", gAccount, gC2dmDeviceId);
   message = soup_message_new("PUT", url);
   g_assert(SOUP_IS_MESSAGE(message));
   g_free(url);

   str = g_strdup_printf("{\n"
                         "  \"device_token\" : \"%064u\",\n"
                         "  \"device_type\" : \"gcm\"\n"
                         "}",
                         gC2dmDeviceId);
   soup_message_headers_set_content_type(message->request_headers, "application/json", NULL);
   soup_message_body_append_take(message->request_body, (guint8 *)str, strlen(str));

   soup_session_queue_message(session, message, put_cb, NULL);

   g_application_run(G_APPLICATION(gApplication), 0, NULL);

   g_clear_object(&gApplication);
}

static void
delete_cb (SoupSession *session,
           SoupMessage *message,
           gpointer     user_data)
{
   g_assert(SOUP_IS_SESSION(session));
   g_assert(SOUP_IS_MESSAGE(message));

   g_assert_cmpint(message->status_code, ==, 204);
   g_assert_cmpstr(message->response_body->data, ==, "");

   g_application_quit(G_APPLICATION(gApplication));
}

static void
test6 (void)
{
   SoupSession *session;
   SoupMessage *message;
   gchar *url;

   gApplication = application_new(G_STRFUNC);

   session = soup_session_async_new();
   g_assert(SOUP_IS_SESSION(session));

   url = g_strdup_printf("http://127.0.0.1:6616/v1/users/%s/devices/%064u", gAccount, gC2dmDeviceId);
   message = soup_message_new("DELETE", url);
   g_assert(SOUP_IS_MESSAGE(message));
   g_free(url);

   soup_session_queue_message(session, message, delete_cb, NULL);

   g_application_run(G_APPLICATION(gApplication), 0, NULL);

   g_clear_object(&gApplication);
}

gint
main (gint argc,
      gchar *argv[])
{
   g_setenv("GSETTINGS_BACKEND", "memory", FALSE);

   g_type_init();
   g_test_init(&argc, &argv, NULL);

   gKeyFile = g_key_file_new();
   g_key_file_load_from_data(gKeyFile, gConfig, -1, 0, NULL);

   gAccount = g_strdup_printf("%024u", g_random_int());

   gC2dmDeviceId = g_random_int();
   gC2dmDevice = g_strdup_printf("{\n"
                                 "  \"device_token\": \"%064u\",\n"
                                 "  \"device_type\": \"c2dm\"\n"
                                 "}", gC2dmDeviceId);

   g_test_add_func("/PostalHttp/get_devices", test1);
   g_test_add_func("/PostalHttp/add_device", test2);
   g_test_add_func("/PostalHttp/get_devices2", test3);
   g_test_add_func("/PostalHttp/get_device", test4);
   g_test_add_func("/PostalHttp/update_device", test5);
   g_test_add_func("/PostalHttp/remove_device", test6);

   return g_test_run();
}
