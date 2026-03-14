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

#pragma once

#include "common/admin_socket.h"

class CephContext;

class RGWHeapProfilerHook : public AdminSocketHook {
  CephContext *cct;

public:
  explicit RGWHeapProfilerHook(CephContext *cct);

  int start();
  void shutdown();

  int call(std::string_view command, const cmdmap_t& cmdmap,
           const bufferlist& inbl,
           Formatter *f,
           std::ostream& ss,
           bufferlist& out) override;
};
