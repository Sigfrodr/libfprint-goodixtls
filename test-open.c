/*
 * Minimal open test for the Goodix GXFP5187 driver
 *
 * Copyright (C) 2026 Benjamin Allègre (https://github.com/Sigfrodr)
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <fprint.h>
#include <stdio.h>
int main(void){
  g_autoptr(FpContext) ctx = fp_context_new();
  GPtrArray *devs = fp_context_get_devices(ctx);
  if (!devs || devs->len==0){ printf("no device found\n"); return 1; }
  FpDevice *d = g_ptr_array_index(devs,0);
  printf("opening %s...\n", fp_device_get_name(d));
  g_autoptr(GError) e=NULL;
  if (!fp_device_open_sync(d,NULL,&e)){ printf("open FAILED: %s\n", e?e->message:"?"); return 1; }
  printf("OPEN OK (see the firmware log above)\n");
  fp_device_close_sync(d,NULL,NULL);
  return 0;
}
