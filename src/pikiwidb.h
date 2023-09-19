/*
 * Copyright (c) 2023-present, Qihoo, Inc.  All rights reserved.
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "cmd_table_manager.h"
#include "event_loop.h"
#include "pstring.h"
#include "tcp_obj.h"

#define PIKIWIDB_VERSION "4.0.0"

class PikiwiDB final {
 public:
  PikiwiDB();
  ~PikiwiDB();

  // 解析输入参数
  bool ParseArgs(int ac, char* av[]);
  // 获取配置文件名
  const pikiwidb::PString& GetConfigName() const { return cfgFile_; }

  // 初始化PikiwiDB
  bool Init();
  // 运行
  void Run();
  // 回收资源
  void Recycle();
  // 停止
  void Stop();

  // 建立一个client连接
  void OnNewConnection(pikiwidb::TcpObject* obj);

  // cmd表管理器
  std::unique_ptr<pikiwidb::CmdTableManager>& CmdTableManager();

 public:
  pikiwidb::EventLoop event_loop_; 

  pikiwidb::PString cfgFile_; // config file
  unsigned short port_; // 
  pikiwidb::PString logLevel_; // log level

  pikiwidb::PString master_;
  unsigned short masterPort_; // 主从同步的端口

  static const unsigned kRunidSize;  // 用于生成runid，来标识是否是同一个pikiwidb实例

 private:
  std::unique_ptr<pikiwidb::CmdTableManager> cmdTableManager_; // cmd表管理器
};

extern std::unique_ptr<PikiwiDB> g_pikiwidb; // 全局的pikiwidb句柄