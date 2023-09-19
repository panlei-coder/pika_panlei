/*
 * Copyright (c) 2023-present, Qihoo, Inc.  All rights reserved.
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

//
//  PikiwiDB.cc

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <iostream>
#include <thread>

#include "log.h"

#include "client.h"
#include "command.h"
#include "store.h"

#include "config.h"
#include "db.h"
#include "pubsub.h"
#include "slow_log.h"

#include "pikiwidb.h"
#include "pikiwidb_logo.h"

std::unique_ptr<PikiwiDB> g_pikiwidb;

// 信号处理函数
static void SignalHandler(int num) {
  if (g_pikiwidb) {
    g_pikiwidb->Stop();
  }
}

// 初始化信号处理
static void InitSignal() {
  struct sigaction sig;
  ::memset(&sig, 0, sizeof(sig));

  // Set SignalHandler as the handler for SIGINT (interrupt signal)
  sig.sa_handler = SignalHandler;
  sigaction(SIGINT, &sig, NULL); // SIGINT->SignalHandler

  // ignore sigpipe
  sig.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &sig, NULL); // SIGPIPE->SIG_IGN
}

// 生成pikiwidb实例唯一标识runid的长度
const unsigned PikiwiDB::kRunidSize = 40;

PikiwiDB::PikiwiDB() : port_(0), masterPort_(0) { cmdTableManager_ = std::make_unique<pikiwidb::CmdTableManager>(); }

PikiwiDB::~PikiwiDB() {}

static void Usage() {
  std::cerr << "Usage:  ./pikiwidb-server [/path/to/redis.conf] [options]\n\
        ./pikiwidb-server -v or --version\n\
        ./pikiwidb-server -h or --help\n\
Examples:\n\
        ./pikiwidb-server (run the server with default conf)\n\
        ./pikiwidb-server /etc/redis/6379.conf\n\
        ./pikiwidb-server --port 7777\n\
        ./pikiwidb-server --port 7777 --slaveof 127.0.0.1 8888\n\
        ./pikiwidb-server /etc/myredis.conf --loglevel verbose\n";
}

// 解析参数
bool PikiwiDB::ParseArgs(int ac, char* av[]) {
  for (int i = 0; i < ac; i++) {
    if (cfgFile_.empty() && ::access(av[i], R_OK) == 0) {
      cfgFile_ = av[i];
      continue;
    } else if (strncasecmp(av[i], "-v", 2) == 0 || strncasecmp(av[i], "--version", 9) == 0) {
      std::cerr << "PikiwiDB Server v=" << PIKIWIDB_VERSION << " bits=" << (sizeof(void*) == 8 ? 64 : 32) << std::endl;

      exit(0);
      return true;
    } else if (strncasecmp(av[i], "-h", 2) == 0 || strncasecmp(av[i], "--help", 6) == 0) {
      Usage();
      exit(0);
      return true;
    } else if (strncasecmp(av[i], "--port", 6) == 0) {
      if (++i == ac) {
        return false;
      }
      port_ = static_cast<unsigned short>(std::atoi(av[i]));
    } else if (strncasecmp(av[i], "--loglevel", 10) == 0) {
      if (++i == ac) {
        return false;
      }
      logLevel_ = std::string(av[i]);
    } else if (strncasecmp(av[i], "--slaveof", 9) == 0) {
      if (i + 2 >= ac) {
        return false;
      }

      master_ = std::string(av[++i]);
      masterPort_ = static_cast<unsigned short>(std::atoi(av[++i]));
    } else {
      std::cerr << "Unknow option " << av[i] << std::endl;
      return false;
    }
  }

  return true;
}

// 定期执行db的保存操作
static void PdbCron() {
  using namespace pikiwidb;

  if (g_qdbPid != -1) {
    return;
  }

  // 如果当前时间大于上次保存时间加上配置的保存时间，并且 PStore 的 dirty_ 大于等于保存更改的配置值
  if (Now() > (g_lastPDBSave + unsigned(g_config.saveseconds)) * 1000UL && PStore::dirty_ >= g_config.savechanges) {
    int ret = fork();
    if (ret == 0) {
      {
        PDBSaver qdb;
        qdb.Save(g_config.rdbfullname.c_str());
        std::cerr << "ServerCron child save rdb done, exiting child\n";
      }  //  make qdb to be destructed before exit
      _exit(0);
    } else if (ret == -1) {
      ERROR("fork qdb save process failed");
    } else {
      g_qdbPid = ret;
    }

    INFO("ServerCron save rdb file {}", g_config.rdbfullname);
  }
}

// 从磁盘加载db
static void LoadDBFromDisk() {
  using namespace pikiwidb;

  PDBLoader loader;
  loader.Load(g_config.rdbfullname.c_str());
}

// 检查执行db保存的子进程的状态
static void CheckChild() {
  using namespace pikiwidb;

  if (g_qdbPid == -1) {
    return;
  }

  int statloc = 0;
  pid_t pid = wait3(&statloc, WNOHANG, nullptr); // 等待（所有）子进程状态改变，并将子进程状态改变，并将子进程的进程id存储在变量pid中

  if (pid != 0 && pid != -1) {
    int exit = WEXITSTATUS(statloc); // 提取子进程的退出状态
    int signal = 0; // 存储子进程的信号值

    if (WIFSIGNALED(statloc)) { // 如果子进程是因为信号而终止
      signal = WTERMSIG(statloc); // 提取子进程终止的信号值
    }

    if (pid == g_qdbPid) { // 如果子进程的进程id等于全局变量g_qdbPid（db保存的子进程）
      PDBSaver::SaveDoneHandler(exit, signal); // 调用 PDBSaver 类的 SaveDoneHandler 方法，处理保存完成的操作
      if (PREPL.IsBgsaving()) { // 如果 PREPL 对象正在进行后台保存操作
        PREPL.OnRdbSaveDone(); // 调用 PREPL 类的 OnRdbSaveDone 方法，处理 RDB 文件保存完成的操作
      } else {
        PREPL.TryBgsave(); // 调用 PREPL 类的 TryBgsave 方法，尝试进行后台保存操作
      }
    } else { // 如果子进程的进程 ID 不等于全局变量 g_qdbPid
      ERROR("{} is not rdb process", pid);
      assert(!!!"Is there any back process except rdb?");
    }
  }
}

// 处理来自client的连接
void PikiwiDB::OnNewConnection(pikiwidb::TcpObject* obj) {
  INFO("New connection from {}", obj->GetPeerIp());

  auto client = std::make_shared<pikiwidb::PClient>(obj);
  obj->SetContext(client);

  client->OnConnect();

  // 定义一个消息处理回调函数 msg_cb，并绑定到 PClient 的 HandlePackets 函数
  // @todo 为什么使用client.get()，而不是client->shared_from_this()
  auto msg_cb = std::bind(&pikiwidb::PClient::HandlePackets, client.get(), std::placeholders::_1, std::placeholders::_2,
                          std::placeholders::_3);
  // 将消息处理回调函数设置给 obj
  obj->SetMessageCallback(msg_cb);
  // 设置当连接断开时的回调函数
  obj->SetOnDisconnect([](pikiwidb::TcpObject* obj) { INFO("disconnect from {}", obj->GetPeerIp()); });
  // 设置 TCP 连接的选项为无延迟模式
  obj->SetNodelay(true);
}

// 初始化基本配置信息
bool PikiwiDB::Init() {
  using namespace pikiwidb;

  char runid[kRunidSize + 1] = "";
  getRandomHexChars(runid, kRunidSize);
  // 将 runid 数组中的字符赋值给全局变量 g_config 的 runid 成员
  g_config.runid.assign(runid, kRunidSize); 

  if (port_ != 0) {
    g_config.port = port_;
  }

  if (!logLevel_.empty()) {
    g_config.loglevel = logLevel_;
  }

  if (!master_.empty()) {
    g_config.masterIp = master_;
    g_config.masterPort = masterPort_;
  }

  // 在指定ip和端口上监听新连接
  if (!event_loop_.Listen(g_config.ip.c_str(), g_config.port,
                          std::bind(&PikiwiDB::OnNewConnection, this, std::placeholders::_1))) {
    ERROR("can not bind socket on port {}", g_config.port);
    return false;
  }

  PCommandTable::Init();
  PCommandTable::AliasCommand(g_config.aliases);
  PSTORE.Init(g_config.databases);
  PSTORE.InitExpireTimer();
  PSTORE.InitBlockedTimer();
  PSTORE.InitEvictionTimer();
  PSTORE.InitDumpBackends();
  PPubsub::Instance().InitPubsubTimer();

  // Only if there is no backend, load rdb
  if (g_config.backend == pikiwidb::BackEndNone) {
    LoadDBFromDisk();
  }

  PSlowLog::Instance().SetThreshold(g_config.slowlogtime);
  PSlowLog::Instance().SetLogLimit(static_cast<std::size_t>(g_config.slowlogmaxlen));

  event_loop_.ScheduleRepeatedly(1000 / pikiwidb::g_config.hz, PdbCron);
  event_loop_.ScheduleRepeatedly(1000, &PReplication::Cron, &PREPL);
  event_loop_.ScheduleRepeatedly(1, CheckChild);

  // master ip
  if (!g_config.masterIp.empty()) {
    PREPL.SetMasterAddr(g_config.masterIp.c_str(), g_config.masterPort);
  }

  // output logo to console
  char logo[512] = "";
  snprintf(logo, sizeof logo - 1, pikiwidbLogo, PIKIWIDB_VERSION, static_cast<int>(sizeof(void*)) * 8,
           static_cast<int>(g_config.port));
  std::cout << logo;

  cmdTableManager_->InitCmdTable();

  return true;
}

// 运行PikiwiDB
void PikiwiDB::Run() {
  event_loop_.SetName("pikiwi-main");
  event_loop_.Run();
  INFO("server exit running");

  Recycle();
}

// 回收资源
void PikiwiDB::Recycle() {
  std::cerr << "PikiwiDB::recycle: server is exiting.. BYE BYE\n";
}

// 停止
void PikiwiDB::Stop() { event_loop_.Stop(); }

// 获取cmd表管理器
std::unique_ptr<pikiwidb::CmdTableManager>& PikiwiDB::CmdTableManager() { return cmdTableManager_; }

// 初始化日志信息
static void InitLogs() {
  logger::Init("logs/pikiwidb_server.log");

#if BUILD_DEBUG
  spdlog::set_level(spdlog::level::debug);
#else
  spdlog::set_level(spdlog::level::info);
#endif
}

int main(int ac, char* av[]) {
  g_pikiwidb = std::make_unique<PikiwiDB>();

  InitSignal();
  InitLogs();
  INFO("pikiwidb server start...");

  // Parse and check whether related configuration Settings exist
  if (!g_pikiwidb->ParseArgs(ac - 1, av + 1)) {
    Usage();
    return -1;
  }

  // load the config file if the config file is existed
  if (!g_pikiwidb->GetConfigName().empty()) {
    if (!LoadPikiwiDBConfig(g_pikiwidb->GetConfigName().c_str(), pikiwidb::g_config)) {
      std::cerr << "Load config file [" << g_pikiwidb->GetConfigName() << "] failed!\n";
      return -1;
    }
  }

  /*
  The current process is turned into a daemon by calling the posix_spawn function. 
  The newly created child process executes the same code as the parent process and 
  inherits the parent's properties and environment.
  @todo 父子进程都接受客户端的连接请求且使用相同的端口号进行监听，不会产生冲突么?
  */
  if (pikiwidb::g_config.daemonize) {
    pid_t pid;
    ::posix_spawn(&pid, av[0], nullptr, nullptr, av, nullptr);
  }

  if (g_pikiwidb->Init()) {
    g_pikiwidb->Run();
  }

  return 0;
}
