// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab ft=cpp

/*
 * Ceph - scalable distributed file system
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 */

#include "rgw_heap_profiler.h"

#include "common/debug.h"
#include "common/ceph_context.h"
#include "perfglue/heap_profiler.h"
#include "include/str_list.h"

#include <string>
#include <vector>
#include <sstream>

#define dout_subsys ceph_subsys_rgw

using namespace std;
using TOPNSPC::common::cmd_getval;

// --------------- constructor ---------------

RGWHeapProfilerHook::RGWHeapProfilerHook(CephContext *cct) : cct(cct) {}

// --------------- start() ---------------

int RGWHeapProfilerHook::start()
{
  AdminSocket *admin_socket = cct->get_admin_socket();

  int r = admin_socket->register_command(
    "heap "
    "name=heapcmd,type=CephChoices,strings="
    "dump|start_profiler|stop_profiler|release|stats "
    "name=value,type=CephString,req=false",
    this,
    "show heap usage info (available only if compiled with tcmalloc)");

  if (r < 0) {
    lderr(cct) << "ERROR: failed to register 'heap' admin socket command (r="
               << r << ")" << dendl;
    return r;
  }
  return 0;
}

// --------------- shutdown() ---------------

void RGWHeapProfilerHook::shutdown()
{
  AdminSocket *admin_socket = cct->get_admin_socket();
  admin_socket->unregister_commands(this);
}

// --------------- call() ---------------

int RGWHeapProfilerHook::call(
  std::string_view command, const cmdmap_t& cmdmap,
  const bufferlist&,
  Formatter *f,
  std::ostream& ss,
  bufferlist& out)
{
  if (command == "heap") {
    if (!ceph_using_tcmalloc()) {
      ss << "could not issue heap profiler command -- not using tcmalloc!";
      return -EOPNOTSUPP;
    }

    string heapcmd;
    cmd_getval(cmdmap, "heapcmd", heapcmd);

    std::vector<string> cmd_vec;
    get_str_vec(heapcmd, cmd_vec);

    string value;
    if (cmd_getval(cmdmap, "value", value)) {
      cmd_vec.push_back(value);
    }

    std::ostringstream outss;
    ceph_heap_profiler_handle_command(cmd_vec, outss);
    out.append(outss.str());
    return 0;
  }

  return -ENOSYS;
}
