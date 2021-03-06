#include "vcable.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <dirent.h>
#include <assert.h>
#include <err.h>

static void
plugin_close(struct vcable_plugin *plugin)
{
   if (!plugin)
      return;

   if (plugin->close)
      plugin->close();
}

static bool
plugin_open(struct vcable_plugin *plugin, const struct vcable_options *options)
{
   if (!plugin || !plugin->open)
      return false;

   if (!plugin->open(options)) {
      warnx("failed to open plugin %s", plugin->name);
      return false;
   }

   return true;
}

static void
plugin_add(struct vcable *vcable, const char *path)
{
   assert(vcable && path);

   size_t index = 0;
   for (index = 0; index < VCABLE_MAX_PLUGINS && vcable->handle[index]; ++index);
   if (VCABLE_MAX_PLUGINS <= index) {
      warnx("maximum amount (%u) of plugins reached, will skip %s", VCABLE_MAX_PLUGINS, path);
      return;
   }

   // dlmopen allows us to load each plugin into own address space, effectivelly making multiple
   // vcable instances work as expected, without needing more plugin API overhead.
   // Requires _GNU_SOURCE though.
   void *handle;
   if (!(handle = dlmopen(LM_ID_NEWLM, path, RTLD_NOW | RTLD_LOCAL))) {
      warn("dlopen(%s)", path);
      return;
   }

   void (*plugin_register)(struct vcable_plugin *out_plugin);
   if (!(plugin_register = dlsym(handle, "plugin_register"))) {
      warn("dlsym(%s, plugin_register)", path);
      goto error0;
   }

   struct vcable_plugin plugin = {0};
   plugin_register(&plugin);

   if (!plugin.version || strcmp(plugin.version, VCABLE_PLUGIN_VERSION)) {
      warnx("version mismatch for plugin %s (%s plugin, %s current)", plugin.name, (plugin.version ? plugin.version : ""), VCABLE_PLUGIN_VERSION);
      goto error0;
   }

   if (!plugin.open || !plugin.close) {
      warnx("plugin %s does not implement required open() and close() functions, plugin will not be loaded", plugin.name);
      goto error0;
   }

   vcable->handle[index] = handle;
   vcable->plugin[index] = plugin;
   warnx("registered plugin %s (%s) %s", plugin.name, plugin.version, plugin.description);
   return;

error0:
   dlclose(handle);
}

void
vcable_set_options(struct vcable *vcable, const struct vcable_options *options)
{
   vcable->options = *options;
   plugin_open(vcable->active, options);
}

bool
vcable_set_plugin(struct vcable *vcable, uint32_t index)
{
   assert(VCABLE_MAX_PLUGINS > index);
   plugin_close(vcable->active);
   vcable->active = NULL;

   if (!index)
      return true;

   if (!vcable->options.name || !vcable->options.write_cb) {
      errx(EXIT_FAILURE, "vcable_set_options should be called at least once before vcable_set_plugin");
      return false;
   }

   if (!plugin_open((vcable->active = &vcable->plugin[index - 1]), &vcable->options)) {
      vcable->active = NULL;
      return false;
   }

   return true;
}

void
vcable_write(struct vcable *vcable, size_t port, const vcable_sample *buffer, size_t num_samples, uint32_t sample_rate)
{
   assert(buffer);

   if (!vcable->active)
      return;

   assert(vcable->active->write);
   vcable->active->write(port, buffer, num_samples, sample_rate);
}

void
vcable_release(struct vcable *vcable)
{
   if (!vcable)
      return;

   plugin_close(vcable->active);

   for (size_t i = 0; i < VCABLE_MAX_PLUGINS; ++i)
      dlclose(vcable->handle);

   *vcable = (struct vcable){0};
}

bool
vcable_init(struct vcable *vcable)
{
   assert(vcable);
   *vcable = (struct vcable){0};

   const char *path = getenv("VCABLE_PATH");
   const char *paths[] = { path, PLUGINS_PATH };
   for (uint32_t i = 0; i < (sizeof(paths) / sizeof(paths[0])); ++i) {
      if (!paths[i])
         continue;

      DIR *d;
      if (!(d = opendir(paths[i]))) {
         warnx("could not open plugins directory: %s", paths[i]);
         continue;
      }

      struct dirent *dir;
      while ((dir = readdir(d))) {
         const char *prefix = "vcable-";
         if (strncmp(dir->d_name, prefix, strlen(prefix)))
            continue;

         char path[4096];
         snprintf(path, sizeof(path), "%s/%s", paths[i], dir->d_name);
         plugin_add(vcable, path);
      }

      closedir(d);
   }

   return true;
}
