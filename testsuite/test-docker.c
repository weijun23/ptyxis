#include <gio/gio.h>

#include "ptyxis-docker-container.h"
#include "ptyxis-docker-provider-private.h"

static PtyxisContainerProvider *
load_fixture (const char *name)
{
  g_autoptr(PtyxisContainerProvider) provider = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autofree char *contents = NULL;
  gboolean r;
  gsize len;

  path = g_build_filename (g_getenv ("G_TEST_SRCDIR"), "docker", name, NULL);
  g_assert_nonnull (path);

  /* A fresh provider per fixture. Merging a second fixture into the same
   * provider would remove everything the first one added.
   */
  provider = ptyxis_docker_provider_new ();
  g_assert_nonnull (provider);

  g_file_get_contents (path, &contents, &len, &error);
  g_assert_no_error (error);
  g_assert_nonnull (contents);

  r = _ptyxis_docker_provider_parse_json (PTYXIS_DOCKER_PROVIDER (provider), contents, &error);
  g_assert_no_error (error);
  g_assert_true (r);

  return g_steal_pointer (&provider);
}

static PtyxisIpcContainer *
find_by_display_name (PtyxisContainerProvider *provider,
                      const char              *display_name)
{
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (provider));

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr(PtyxisIpcContainer) container = g_list_model_get_item (G_LIST_MODEL (provider), i);

      if (g_strcmp0 (display_name, ptyxis_ipc_container_get_display_name (container)) == 0)
        return g_steal_pointer (&container);
    }

  return NULL;
}

/* Every fixture must parse cleanly. */
static void
test_docker_json (void)
{
  g_autoptr(GDir) dir = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *dirname = NULL;
  const char *name;

  dirname = g_build_filename (g_getenv ("G_TEST_SRCDIR"), "docker", NULL);
  g_assert_nonnull (dirname);

  dir = g_dir_open (dirname, 0, &error);
  g_assert_no_error (error);
  g_assert_nonnull (dir);

  while ((name = g_dir_read_name (dir)))
    {
      g_autoptr(PtyxisContainerProvider) provider = NULL;

      g_debug ("Parsing `%s`", name);

      provider = load_fixture (name);
      g_assert_nonnull (provider);
    }
}

/* `docker container inspect` reports names with a leading slash, and a
 * container may have no name at all.
 */
static void
test_docker_names (void)
{
  g_autoptr(PtyxisContainerProvider) provider = load_fixture ("test1.json");
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (provider));

  g_assert_cmpuint (n_items, >, 0);

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr(PtyxisIpcContainer) container = g_list_model_get_item (G_LIST_MODEL (provider), i);
      const char *display_name = ptyxis_ipc_container_get_display_name (container);

      g_assert_nonnull (display_name);
      g_assert_cmpint (display_name[0], !=, '/');
      g_assert_cmpint (display_name[0], !=, 0);

      g_assert_cmpstr (ptyxis_ipc_container_get_provider (container), ==, "docker");
      g_assert_cmpstr (ptyxis_ipc_container_get_icon_name (container), ==, "container-generic-symbolic");
    }

  {
    g_autoptr(PtyxisIpcContainer) builder = find_by_display_name (provider, "chaotic-builder");
    g_autoptr(PtyxisIpcContainer) watchtower = find_by_display_name (provider, "watchtower");
    /* A container with no Name falls back to a docker style short identifier. */
    g_autoptr(PtyxisIpcContainer) unnamed = find_by_display_name (provider, "112233445566");

    g_assert_nonnull (builder);
    g_assert_nonnull (watchtower);
    g_assert_nonnull (unnamed);

    g_assert_cmpstr (ptyxis_ipc_container_get_id (builder), ==,
                     "f0d4ddcece301024bfc912c859458330d98e6b5aa7f947b456f96d2bf7e91136");
  }
}

/* Label values are allowed to contain commas. This is the whole reason the
 * provider inspects containers rather than reading `docker ps --format json`,
 * whose flattened label string cannot be split unambiguously.
 */
static void
test_docker_labels (void)
{
  g_autoptr(PtyxisContainerProvider) provider = load_fixture ("test1.json");
  g_autoptr(PtyxisIpcContainer) container = find_by_display_name (provider, "watchtower");
  const char *chain;

  g_assert_nonnull (container);
  g_assert_true (PTYXIS_IS_DOCKER_CONTAINER (container));

  chain = ptyxis_docker_container_lookup_label (PTYXIS_DOCKER_CONTAINER (container),
                                                "com.centurylinklabs.watchtower.container-chain");
  g_assert_nonnull (chain);
  g_assert_cmpstr (chain,
                   ==,
                   "1f7c4e0652fae056bb6b1ec81440fb1cb0e5772dde56377484ef53e8868eaa25,"
                   "c666baff6cd3aa14218aa057011564f8f20d2b3173c8500e3b64aa46f88710b5,"
                   "cdf7616ea23dbcc5ca8c8f96b0106750be069a7d7a230ff4b4852995cf7078b3");

  g_assert_cmpstr (ptyxis_docker_container_lookup_label (PTYXIS_DOCKER_CONTAINER (container),
                                                         "com.centurylinklabs.watchtower"),
                   ==, "true");
  g_assert_null (ptyxis_docker_container_lookup_label (PTYXIS_DOCKER_CONTAINER (container),
                                                       "does.not.exist"));
}

/* Docker emits JSON null, not an empty object, for a container with no
 * labels. It must not take the provider down with it.
 */
static void
test_docker_null_labels (void)
{
  g_autoptr(PtyxisContainerProvider) provider = load_fixture ("test1.json");
  g_autoptr(PtyxisIpcContainer) container = find_by_display_name (provider, "no-labels-stopped");

  g_assert_nonnull (container);
  g_assert_true (PTYXIS_IS_DOCKER_CONTAINER (container));
  g_assert_null (ptyxis_docker_container_lookup_label (PTYXIS_DOCKER_CONTAINER (container), "anything"));
}

/* An element with no Id is unusable but must not abort the whole refresh. */
static void
test_docker_no_id (void)
{
  g_autoptr(PtyxisContainerProvider) provider = load_fixture ("test1.json");

  /* test1.json holds six elements, one of which has no Id. */
  g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (provider)), ==, 5);
  g_assert_null (find_by_display_name (provider, "container-without-an-id"));
}

static void
test_docker_empty (void)
{
  g_autoptr(PtyxisContainerProvider) provider = load_fixture ("test2-empty.json");

  g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (provider)), ==, 0);
}

/* `docker ps --quiet --no-trunc` output is just identifiers, one per line. */
static void
test_docker_ids (void)
{
  static const struct {
    const char *input;
    const char *expected[4];
  } tests[] = {
    { "a\nb\n", { "a", "b", NULL } },
    { "a\nb", { "a", "b", NULL } },
    { "", { NULL } },
    { "\n\n", { NULL } },
    { "  a  \n\n  b  \n", { "a", "b", NULL } },
    { "\n", { NULL } },
  };

  for (guint i = 0; i < G_N_ELEMENTS (tests); i++)
    {
      g_auto(GStrv) ids = _ptyxis_docker_provider_parse_ids (tests[i].input);
      guint n_expected = 0;

      g_assert_nonnull (ids);

      while (tests[i].expected[n_expected] != NULL)
        n_expected++;

      g_assert_cmpuint (g_strv_length (ids), ==, n_expected);

      for (guint j = 0; j < n_expected; j++)
        g_assert_cmpstr (ids[j], ==, tests[i].expected[j]);
    }

  {
    g_auto(GStrv) ids = _ptyxis_docker_provider_parse_ids (NULL);

    g_assert_nonnull (ids);
    g_assert_cmpuint (g_strv_length (ids), ==, 0);
  }
}

int
main (int argc,
      char *argv[])
{
  if (g_getenv ("G_TEST_SRCDIR") == NULL)
    g_error ("G_TEST_SRCDIR must be set!");
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/Ptyxis/Docker/JSON", test_docker_json);
  g_test_add_func ("/Ptyxis/Docker/Names", test_docker_names);
  g_test_add_func ("/Ptyxis/Docker/Labels", test_docker_labels);
  g_test_add_func ("/Ptyxis/Docker/NullLabels", test_docker_null_labels);
  g_test_add_func ("/Ptyxis/Docker/NoId", test_docker_no_id);
  g_test_add_func ("/Ptyxis/Docker/Empty", test_docker_empty);
  g_test_add_func ("/Ptyxis/Docker/Ids", test_docker_ids);
  return g_test_run ();
}
