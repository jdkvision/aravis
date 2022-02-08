/* Aravis - Digital camera library
 *
 * Copyright © 2009-2019 Emmanuel Pacaud
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: Emmanuel Pacaud <emmanuel@gnome.org>
 */

#include <arvsystem.h>
#include <arvgvinterfaceprivate.h>
#include <arvfeatures.h>
#if ARAVIS_HAS_USB
#include <arvuvinterfaceprivate.h>
#endif
#include <arvfakeinterfaceprivate.h>
#include <arvdevice.h>
#include <arvdebugprivate.h>
#include <string.h>
#include <arvmisc.h>
#include <arvdomimplementation.h>

#include <glib/gstdio.h>

static GMutex arv_system_mutex;

/**
 * SECTION: arv
 * @short_description: Device discovery and instantiation
 *
 * This module contans a set of APIs that allows to list and enable/disable the available interfaces,
 * and list and instantiate devices.
 */

ArvInterfaceInfos interfaces[] = {
	{
		.interface_id = "Fake",
		.is_available = FALSE,
		.get_interface_instance = arv_fake_interface_get_instance,
		.destroy_interface_instance =  arv_fake_interface_destroy_instance
	},
#if ARAVIS_HAS_USB
	{	.interface_id = "USB3Vision",
		.is_available = TRUE,
		.get_interface_instance = arv_uv_interface_get_instance,
		.destroy_interface_instance = arv_uv_interface_destroy_instance
	},
#endif
	{	.interface_id = "GigEVision",
		.is_available = TRUE,
		.get_interface_instance = arv_gv_interface_get_instance,
		.destroy_interface_instance = arv_gv_interface_destroy_instance
	}
};

typedef struct {
   GModule *module;
   ArvInterfaceInfos *infos;
}ArvInterfaceExtension;

typedef ArvInterfaceInfos* (*AravisExtensionEntryPoint) (void);

static GList *interface_extension_process_file(GList *interface_extension_list,
                                               const gchar *filename)
{
   if( g_file_test(filename, G_FILE_TEST_IS_REGULAR) )
   {
      GStatBuf file_status;
      if( !g_stat(filename, &file_status) )
      {
         GModuleFlags flags = G_MODULE_BIND_LOCAL;

         GModule *module = g_module_open (filename, flags);
         if( module )
         {
            gpointer ptr;
            if( g_module_symbol(module, "aravis_extension_entry", &ptr) )
            {
               ArvInterfaceInfos *infos;
               AravisExtensionEntryPoint entry_func;

               entry_func = (AravisExtensionEntryPoint)ptr;

               g_module_make_resident(module);

               infos = entry_func();
               if( infos )
               {
                 if( infos->interface_id &&
                     infos->get_interface_instance &&
                     infos->destroy_interface_instance )
                 {
                    ArvInterfaceExtension *entry = g_malloc(sizeof(ArvInterfaceExtension));
                    entry->module = module;
                    entry->infos = infos;
                    interface_extension_list = g_list_append(interface_extension_list, entry);
                 }
                 else
                 {
                    g_warning("\"aravis_extension_entry\" in %s returned incomplete ArvInterfaceInfos", filename);
                    g_module_close (module);
                 }
               }
               else
               {
                  g_warning("\"aravis_extension_entry\" in %s returned NULL", filename);
                  g_module_close (module);
               }
            }
            else
            {
               g_warning("\"aravis_extension_entry\" symbol not found in %s",
                         filename);
               g_module_close (module);
            }
         }
         else
         {
            g_warning("module_open failed for filename %s: %s",
                      filename, g_module_error ());
         }
      }
      else
      {
         g_warning("Problem accessing file %s", filename);
      }
   }
   else
   {
      g_warning("Skipping %s", filename);
   }

   return interface_extension_list;
}

static GList *interface_extension_process_dir(GList *interface_extension_list,
                                              gchar *dirname)
{
   GError *err;
   GDir *dir = g_dir_open(dirname,
                          0,
                          &err);
   if( dir )
   {
      const gchar *filename;
      while((filename = g_dir_read_name(dir)))
      {
         gchar *fullfilename = g_strconcat(dirname, "/", filename, NULL);
         interface_extension_list = interface_extension_process_file(interface_extension_list,
                                                                     fullfilename);
         g_free(fullfilename);
      }
      g_dir_close(dir);
   }
   else
   {
      g_warning("g_dir_open(%s) failed", dirname);
      if( err && err->message)
      {
         g_warning("%s", err->message);
         g_clear_error(&err);
      }
   }

   return interface_extension_list;
}

//arv_system_mutex should be locked when calling this
static GList* get_interface_extension_list(void)
{
   static GList *interface_extension_list = NULL;
   static gboolean bpopulated = FALSE;
   if( !bpopulated )
   {
      gchar **env;
      const gchar *pluginpathenv;
      bpopulated = TRUE;

      //we will populate a list of ArvInterfaceExtension's by loading
      // plugins sitting in folders specified by ARAVIS_PLUGIN_PATH
      env = g_get_environ();
      pluginpathenv = g_environ_getenv(env,"ARAVIS_PLUGIN_PATH");
      if( pluginpathenv )
      {
         if (g_module_supported ())
         {
            gchar **dirtokens = g_strsplit(pluginpathenv,
                                       ":",
                                       -1);
            if( dirtokens )
            {
               gint diri = 0;
               gchar *dirname = dirtokens[diri++];
               while( (dirname != NULL) && (*dirname) )
               {
                  interface_extension_list = interface_extension_process_dir(interface_extension_list,
                                                                             dirname);
                  dirname = dirtokens[diri++];
               }

               g_strfreev(dirtokens);
            }
         }
         else
         {
            g_warning ("[Arv::get_interface_extension_list] interface loading not supported");
         }
      }
      g_strfreev(env);
   }

   return interface_extension_list;
}

/**
 * arv_get_n_interfaces:
 *
 * Gets the number of available interfaces, including the disabled ones.
 *
 * Returns: The number of available interfaces.
 */

unsigned int
arv_get_n_interfaces (void)
{
   unsigned int ninterfaces = 0;
   GList *interface_extension_list;
   g_mutex_lock (&arv_system_mutex);
   interface_extension_list = get_interface_extension_list();
   ninterfaces = g_list_length(interface_extension_list) + G_N_ELEMENTS (interfaces);
   g_mutex_unlock (&arv_system_mutex);

	return ninterfaces;
}


/**
 * arv_get_interface_id:
 * @index: interface index
 *
 * Retrieves the interface identifier. Possible values are 'Fake', 'USB3Vision'
 * and 'GigEVision'.
 *
 * Returns: The interface identifier string.
 */

const char *
arv_get_interface_id (unsigned int index)
{
   const char *interface_id = NULL;
   GList *interface_extension_list;
   ArvInterfaceExtension *entry;
	if (index >= arv_get_n_interfaces())
		return NULL;

	if( index < G_N_ELEMENTS (interfaces) )
	   return interfaces[index].interface_id;

	index -= G_N_ELEMENTS (interfaces);
	g_mutex_lock (&arv_system_mutex);
   interface_extension_list = get_interface_extension_list();
   entry = (ArvInterfaceExtension *)g_list_nth_data(interface_extension_list,
                                                  index);
   if( entry && entry->infos)
   {
      interface_id = entry->infos->interface_id;
   }
	g_mutex_unlock (&arv_system_mutex);

	return interface_id;
}

/**
 * arv_enable_interface:
 * @interface_id: name of the interface
 *
 * Enable an interface by name. By default, all interfaces are enabled, except 'Fake'.
 */

void
arv_enable_interface (const char *interface_id)
{
	guint i;
	GList *interface_extension_list;

	g_return_if_fail (interface_id != NULL);

	for (i = 0; i < G_N_ELEMENTS (interfaces) ; i++)
		if (strcmp (interface_id, interfaces[i].interface_id) == 0) {
			interfaces[i].is_available = TRUE;
			return;
		}

	g_mutex_lock (&arv_system_mutex);
   interface_extension_list = get_interface_extension_list();
   while( interface_extension_list )
   {
      ArvInterfaceExtension *entry = interface_extension_list->data;
      if( entry && entry->infos )
      {
         if (strcmp (interface_id, entry->infos->interface_id) == 0) {
            entry->infos->is_available = TRUE;
            g_mutex_unlock (&arv_system_mutex);
            return;
         }
      }
      interface_extension_list = g_list_next(interface_extension_list);
   }
	g_mutex_unlock (&arv_system_mutex);

	g_warning ("[Arv::enable_interface] Unknown interface '%s'", interface_id);
}

/**
 * arv_disable_interface:
 * @interface_id: name of the interface
 *
 * Disable an interface by name. By default, all interfaces are enabled, except 'Fake'.
 */

void
arv_disable_interface (const char *interface_id)
{
	guint i;
	GList *interface_extension_list;

	g_return_if_fail (interface_id != NULL);

	for (i = 0; i < G_N_ELEMENTS (interfaces); i++)
		if (strcmp (interface_id, interfaces[i].interface_id) == 0) {
			interfaces[i].is_available = FALSE;
			return;
		}

	g_mutex_lock (&arv_system_mutex);
   interface_extension_list = get_interface_extension_list();
   while( interface_extension_list )
   {
      ArvInterfaceExtension *entry = interface_extension_list->data;
      if( entry && entry->infos )
      {
         if (strcmp (interface_id, entry->infos->interface_id) == 0) {
            entry->infos->is_available = FALSE;
            g_mutex_unlock (&arv_system_mutex);
            return;
         }
      }
      interface_extension_list = g_list_next(interface_extension_list);
   }
   g_mutex_unlock (&arv_system_mutex);

	g_warning ("[Arv::enable_interface] Unknown interface '%s'", interface_id);
}

/**
 * arv_update_device_list:
 *
 * Updates the list of currently online devices.
 **/

void
arv_update_device_list (void)
{
	unsigned int i;
	GList *interface_extension_list;

	g_mutex_lock (&arv_system_mutex);

	for (i = 0; i < G_N_ELEMENTS (interfaces); i++) {
		ArvInterface *interface;

		if (interfaces[i].is_available) {
			interface = interfaces[i].get_interface_instance ();
			arv_interface_update_device_list (interface);
		}
	}

	interface_extension_list = get_interface_extension_list();
	while( interface_extension_list )
   {
	   ArvInterface *interface;
      ArvInterfaceExtension *entry = interface_extension_list->data;
      if( entry && entry->infos )
      {
         if (entry->infos->is_available && entry->infos->get_interface_instance) {
            interface = entry->infos->get_interface_instance();
            arv_interface_update_device_list (interface);
         }
      }
      interface_extension_list = g_list_next(interface_extension_list);
   }

	g_mutex_unlock (&arv_system_mutex);
}

/**
 * arv_get_n_devices:
 *
 * Retrieves the number of currently online devices. This value is valid until
 * the next call to arv_update_device_list().
 *
 * Returns: The number of currently online devices.
 **/

unsigned int
arv_get_n_devices (void)
{
	unsigned int n_devices = 0;
	unsigned int i;
	GList *interface_extension_list;

	g_mutex_lock (&arv_system_mutex);

	for (i = 0; i < G_N_ELEMENTS (interfaces); i++) {
		ArvInterface *interface;

		if (interfaces[i].is_available) {
			interface = interfaces[i].get_interface_instance ();
			n_devices += arv_interface_get_n_devices (interface);
		}
	}

	interface_extension_list = get_interface_extension_list();
   while( interface_extension_list )
   {
      ArvInterface *interface;
      ArvInterfaceExtension *entry = interface_extension_list->data;
      if( entry && entry->infos )
      {
         if (entry->infos->is_available && entry->infos->get_interface_instance) {
            interface = entry->infos->get_interface_instance();
            n_devices += arv_interface_get_n_devices (interface);
         }
      }
      interface_extension_list = g_list_next(interface_extension_list);
   }

	g_mutex_unlock (&arv_system_mutex);

	return n_devices;
}

static const char *
arv_get_info (unsigned int index, const char *get_info (ArvInterface *, guint))
{
	unsigned int offset = 0;
	unsigned int i;
	const char *info;
	GList *interface_extension_list;

	g_mutex_lock (&arv_system_mutex);

	for (i = 0; i < G_N_ELEMENTS (interfaces); i++) {
		ArvInterface *interface;
		unsigned int n_devices;

		if (interfaces[i].is_available) {
			interface = interfaces[i].get_interface_instance ();
			n_devices = arv_interface_get_n_devices (interface);

			if (index - offset < n_devices) {
				info = get_info (interface, index - offset);

				g_mutex_unlock (&arv_system_mutex);

				return info;
			}

			offset += n_devices;
		}
	}

	interface_extension_list = get_interface_extension_list();
   while( interface_extension_list )
   {
      ArvInterfaceExtension *entry;
      ArvInterface *interface;
      unsigned int n_devices;

      entry = interface_extension_list->data;
      if( entry && entry->infos )
      {
         if (entry->infos->is_available && entry->infos->get_interface_instance) {
            interface = entry->infos->get_interface_instance();
            n_devices = arv_interface_get_n_devices (interface);
            if (index - offset < n_devices) {
               info = get_info (interface, index - offset);

               g_mutex_unlock (&arv_system_mutex);

               return info;
            }
         }
      }
      interface_extension_list = g_list_next(interface_extension_list);
   }

	g_mutex_unlock (&arv_system_mutex);

	return NULL;
}

/**
 * arv_get_device_id:
 * @index: device index
 *
 * Queries the unique device id corresponding to index.  Prior to this
 * call the arv_update_device_list() function must be called.
 *
 * Returns: a unique device id
 *
 * Since: 0.2.0
 */

const char *
arv_get_device_id (unsigned int index)
{
	return arv_get_info (index, arv_interface_get_device_id);
}

/**
 * arv_get_device_physical_id:
 * @index: device index
 *
 * Queries the physical device id corresponding to index such
 * as the MAC address for Ethernet based devices, bus id for PCI,
 * USB or Firewire based devices.
 *
 * Prior to this call the arv_update_device_list()
 * function must be called.
 *
 * Returns: a physical device id
 *
 * Since: 0.2.0
 */

const char *
arv_get_device_physical_id (unsigned int index)
{
	return arv_get_info (index, arv_interface_get_device_physical_id);
}

/**
 * arv_get_device_vendor:
 * @index: device index
 *
 * Queries the device vendor.
 *
 * Prior to this call the arv_update_device_list()
 * function must be called.
 *
 * Returns: (transfer none): the device vendor, NULL on error
 *
 * Since: 0.6.0
 */

const char *
arv_get_device_vendor	(unsigned int index)
{
	return arv_get_info (index, arv_interface_get_device_vendor);
}

/**
 * arv_get_device_manufacturer_info:
 * @index: device index
 *
 * Queries the device manufacturer info.
 *
 * Prior to this call the arv_update_device_list()
 * function must be called.
 *
 * Returns: (transfer none): the device manufacturer info, NULL on error
 *
 * Since: 0.8.20
 */

const char *
arv_get_device_manufacturer_info (unsigned int index)
{
	return arv_get_info (index, arv_interface_get_device_manufacturer_info);
}

/**
 * arv_get_device_model:
 * @index: device index
 *
 * Queries the device model.
 *
 * Prior to this call the arv_update_device_list()
 * function must be called.
 *
 * Returns: (transfer none): the device model, NULL on error
 *
 * Since: 0.6.0
 */

const char *
arv_get_device_model (unsigned int index)
{
	return arv_get_info (index, arv_interface_get_device_model);
}

/**
 * arv_get_device_serial_nbr:
 * @index: device index
 *
 * Queries the device serial.
 *
 * Prior to this call the arv_update_device_list()
 * function must be called.
 *
 * Returns: (transfer none): the device serial, NULL on error
 *
 * Since: 0.6.0
 */

const char *
arv_get_device_serial_nbr (unsigned int index)
{
	return arv_get_info (index, arv_interface_get_device_serial_nbr);
}

/**
 * arv_get_device_address:
 * @index: device index
 *
 * The index of a device may change after a call to arv_update_device_list().
 *
 * Returns: The address of the device corresponding to @index as a string.
 *
 * Since: 0.6.0
 */

const char *
arv_get_device_address (unsigned int index)
{
	return arv_get_info (index, arv_interface_get_device_address);
}

/**
 * arv_get_device_protocol:
 * @index: device index
 *
 * The index of a device may change after a call to arv_update_device_list().
 *
 * Returns: The device protocol as a string.
 *
 * Since: 0.6.0
 */

const char *
arv_get_device_protocol	(unsigned int index)
{
	return arv_get_info (index, arv_interface_get_device_protocol);
}

/**
 * arv_open_device:
 * @device_id: (allow-none): a device identifier string
 * @error: a #GError placeholder, %NULL to ignore
 *
 * Open a device corresponding to the given identifier. A %NULL string makes
 * this function return the first available device.
 *
 * Return value: (transfer full): A new #ArvDevice instance.
 *
 * Since: 0.8.0
 */

ArvDevice *
arv_open_device (const char *device_id, GError **error)
{
	unsigned int i;
	GList *interface_extension_list;

	g_mutex_lock (&arv_system_mutex);

	for (i = 0; i < G_N_ELEMENTS (interfaces); i++) {
		ArvInterface *interface;
		ArvDevice *device;

		if (interfaces[i].is_available) {
			GError *local_error = NULL;

			interface = interfaces[i].get_interface_instance ();
			device = arv_interface_open_device (interface, device_id, &local_error);
			if (ARV_IS_DEVICE (device) || local_error != NULL) {
				if (local_error != NULL)
					g_propagate_error (error, local_error);
				g_mutex_unlock (&arv_system_mutex);
				return device;
			}
		}
	}

	interface_extension_list = get_interface_extension_list();
   while( interface_extension_list )
   {
      ArvInterfaceExtension *entry;
      ArvInterface *interface;
      ArvDevice *device;

      entry = interface_extension_list->data;
      if( entry && entry->infos )
      {
         if (entry->infos->is_available && entry->infos->get_interface_instance) {
            GError *local_error = NULL;
            interface = entry->infos->get_interface_instance();
            device = arv_interface_open_device (interface, device_id, &local_error);
            if (ARV_IS_DEVICE (device) || local_error != NULL) {
               if (local_error != NULL)
                  g_propagate_error (error, local_error);
               g_mutex_unlock (&arv_system_mutex);
               return device;
            }
         }
      }
      interface_extension_list = g_list_next(interface_extension_list);
   }

	g_mutex_unlock (&arv_system_mutex);

	if (device_id != NULL)
		g_set_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_FOUND,
			     "Device '%s' not found", device_id);
	else
		g_set_error (error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_NOT_FOUND,
			     "No supported device found");

	return NULL;
}

/**
 * arv_shutdown:
 *
 * Frees a number of ressources allocated by Aravis that would be otherwise
 * reported as memory leak by tools like Valgrind. The call to this function is
 * optional if you don't intend to check for memory leaks.
 */

void
arv_shutdown (void)
{
	unsigned int i;
	GList *interface_extension_list;

	g_mutex_lock (&arv_system_mutex);

	for (i = 0; i < G_N_ELEMENTS (interfaces); i++)
		interfaces[i].destroy_interface_instance ();

	interface_extension_list = get_interface_extension_list();
   while( interface_extension_list )
   {
      ArvInterfaceExtension *entry;
      entry = interface_extension_list->data;
      if( entry )
      {
         if( entry->infos )
         {
            if( entry->infos->destroy_interface_instance )
            {
               entry->infos->destroy_interface_instance();
            }

            g_free(entry->infos);
         }

         if( entry->module )
            g_module_close(entry->module);

         g_free(entry);
      }

      interface_extension_list = g_list_next(interface_extension_list);
   }

   //TODO: Need to destroy the list..

	arv_dom_implementation_cleanup ();

	g_mutex_unlock (&arv_system_mutex);
}
