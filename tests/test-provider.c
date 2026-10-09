/*
 * test-provider.c: checks that the groupwise Camel provider loads and
 * creates a configured store.
 *
 * Usage: test-provider [MODULE-PATH]
 *   With MODULE-PATH the module is loaded directly (build tree); without,
 *   Camel has to find it on its own (installed, or via EDS_EXTRA_PREFIXES).
 */

#include <string.h>
#include <glib/gstdio.h>

#include <camel/camel.h>

static gchar *
make_temp_dir (void)
{
	gchar *dir = g_dir_make_tmp ("gw-test-XXXXXX", NULL);

	g_assert_nonnull (dir);
	return dir;
}

static void
remove_tree (const gchar *path)
{
	GDir *dir = g_dir_open (path, 0, NULL);
	const gchar *entry;

	while (dir && (entry = g_dir_read_name (dir)) != NULL) {
		gchar *child = g_build_filename (path, entry, NULL);

		if (g_file_test (child, G_FILE_TEST_IS_DIR))
			remove_tree (child);
		else
			g_unlink (child);
		g_free (child);
	}
	if (dir)
		g_dir_close (dir);
	g_rmdir (path);
}

int
main (int argc,
      char **argv)
{
	GError *error = NULL;
	CamelProvider *provider;
	CamelSession *session;
	CamelService *service;
	CamelSettings *settings;
	gchar *tmp, *name;

	g_log_set_always_fatal (G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING);

	camel_init (NULL, FALSE);
	camel_provider_init ();

	if (argc > 1 && !camel_provider_load (argv[1], &error))
		g_error ("Cannot load %s: %s", argv[1], error->message);

	provider = camel_provider_get ("groupwise", &error);
	if (!provider)
		g_error ("Provider 'groupwise' not found: %s", error->message);

	g_assert_cmpstr (provider->protocol, ==, "groupwise");
	g_assert_cmpstr (provider->domain, ==, "mail");
	g_assert_true (provider->object_types[CAMEL_PROVIDER_STORE] != G_TYPE_INVALID);
	g_print ("provider: %s (%s)\n", provider->name, provider->description);

	tmp = make_temp_dir ();
	session = g_object_new (CAMEL_TYPE_SESSION,
		"user-data-dir", tmp,
		"user-cache-dir", tmp,
		NULL);

	service = camel_session_add_service (session, "gw-test", "groupwise", CAMEL_PROVIDER_STORE, &error);
	if (!service)
		g_error ("Cannot create store: %s", error->message);

	g_assert_true (CAMEL_IS_OFFLINE_STORE (service));
	g_assert_true (CAMEL_IS_NETWORK_SERVICE (service));
	g_assert_cmpuint (camel_network_service_get_default_port (
		CAMEL_NETWORK_SERVICE (service), CAMEL_NETWORK_SECURITY_METHOD_NONE), ==, 7191);

	settings = camel_service_ref_settings (service);
	g_assert_true (CAMEL_IS_NETWORK_SETTINGS (settings));
	g_assert_true (CAMEL_IS_OFFLINE_SETTINGS (settings));
	g_object_set (settings, "host", "gw.example.com", "user", "tester", "port", 7191, NULL);
	g_object_unref (settings);

	name = camel_service_get_name (service, FALSE);
	g_print ("store: %s\n", name);
	g_assert_nonnull (strstr (name, "gw.example.com"));
	g_free (name);

	g_object_unref (service);
	g_object_unref (session);
	remove_tree (tmp);
	g_free (tmp);

	g_print ("OK\n");
	return 0;
}
