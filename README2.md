# AI 云存储系统 README

## 1. 项目简介

AI 云存储系统是一个私有云文件管理平台，支持用户登录注册、文件上传下载、文件分享、图床分享、分片上传、秒传和 AI 语义检索。

当前项目使用 Docker Compose 在本机模拟一套 FastDFS 集群：

- 2 个 FastDFS Tracker：负责维护 storage 状态、为上传和下载分配节点。
- 4 个 FastDFS Storage：分成 `group1` 和 `group2` 两组。
- 1 个 Nginx 入口：部署在 `storage1` 容器中，对外提供 HTTPS、前端静态页面、API 反向代理和 FastDFS 文件访问。
- 1 个 FastCGI 应用容器：运行 C/C++ 写的业务 CGI 服务。
- 1 个 MySQL 容器：保存用户、文件、分享等业务元数据。

### 技术栈

| 层级 | 技术 |
| --- | --- |
| 前端 | React 18、Ant Design 5 |
| 后端 | C/C++ FastCGI、spawn-fcgi |
| Web 入口 | Nginx 1.20.2、HTTPS 自签名证书 |
| 文件存储 | FastDFS 6.06、fastdfs-nginx-module 1.22 |
| 数据库 | MySQL 8.0 |
| 缓存 | Redis，运行在 `fastcgi_app` 容器内部 |
| AI 检索 | DashScope API、FAISS 1.7.2 |
| 容器编排 | Docker Compose |

### 核心功能

- 用户注册/登录（加盐 MD5 密码哈希）
- 文件上传（普通上传 + 大文件分片上传，支持断点续传）
- 文件秒传（基于 MD5 去重）
- 文件下载、删除、分享
- 共享文件广场 + 下载排行榜
- 图床分享功能（生成提取码）
- AI 智能语义搜索（自然语言查找文件）
- AI 自动图片描述（Qwen-VL 多模态模型）

## 2. 当前容器架构

```text
┌──────────────────────────────────────────────────────────────┐
│                    Client / Browser                          │
│                 React 18 + Ant Design                        │
└───────────────────────┬──────────────────────────────────────┘
                        │ HTTPS :443
                        ▼
┌──────────────────────────────────────────────────────────────┐
│              tc_fcgi_storage1  (172.30.0.5)                  │
│                                                              │
│  ┌──────────────────────┐   ┌─────────────────────────────┐  │
│  │      Nginx           │   │     FastDFS Storage         │  │
│  │    80 / 443          │   │     group1 / storage1       │  │
│  │    SSL Termination   │   │     port 23000              │  │
│  └──────────┬───────────┘   └─────────────────────────────┘  │
│             │                                                │
│   ┌─────────┴────────────────────────────────────────────┐   │
│   │ 路由规则                                             │   │
│   │                                                      │   │
│   │  /app/front        -> React 静态资源                 │   │
│   │  /group1/M00/...   -> 本机 fastdfs-nginx-module      │   │
│   │  /group2/M00/...   -> 转发到 storage3(172.30.0.7)    │   │
│   │  /api/*            -> FastCGI                        │   │
│   └──────────────────────────────────────────────────────┘   │
└───────────────────────┬──────────────────────────────────────┘
                        │ FastCGI
                        ▼
┌──────────────────────────────────────────────────────────────┐
│                 tc_fcgi_app  (172.30.0.9)                    │
│                                                              │
│  ┌──────────────────────┐                                    │
│  │       Redis          │                                    │
│  │      127.0.0.1       │                                    │
│  │   Session / Cache    │                                    │
│  └──────────────────────┘                                    │
│                                                              │
│  ┌────────────────────────────────────────────────────────┐  │
│  │                FastCGI Service Layer                   │  │
│  │                                                        │  │
│  │  login              register                           │  │
│  │  upload             md5                                │  │
│  │  myfiles            dealfile                           │  │
│  │  sharefiles         dealsharefile                      │  │
│  │  sharepicture       ai                                 │  │
│  │  chunk_init         chunk_upload                       │  │
│  │  chunk_merge                                           │  │
│  └────────────────────────────────────────────────────────┘  │
└──────────────┬───────────────┬───────────────┬───────────────┘
               │               │               │
               ▼               ▼               ▼
    ┌────────────────┐  ┌────────────────┐  ┌────────────────┐
    │ MySQL 8.0      │  │ DashScope API  │  │ FAISS Index    │
    │ 172.30.0.2     │  │ Qwen-VL        │  │ /data/faiss/   │
    │ Metadata Store │  │ Embedding API  │  │ Vector Search  │
    └────────────────┘  └────────────────┘  └────────────────┘
               │
               ▼
    ┌─────────────────────────────────────────────────────┐
    │               FastDFS Client SDK                    │
    │                 /etc/fdfs/client                    │
    └──────────────────────┬──────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│                     FastDFS Cluster                         │
│                                                             │
│  tracker1 : 172.30.0.3:22122                                │
│  tracker2 : 172.30.0.4:22122                                │
│                                                             │
│  group1 : storage1 / storage2                               │
│  group2 : storage3 / storage4                               │
└─────────────────────────────────────────────────────────────┘
```

## 3. 容器列表

| 容器名 | Compose 服务 | IP | 对外端口 | 作用 |
| --- | --- | --- | --- | --- |
| `tc_fcgi_mysql` | `tc_mysql` | `172.30.0.2` | 无 | MySQL 数据库，保存业务元数据 |
| `tc_fcgi_tracker1` | `tracker1` | `172.30.0.3` | 无 | FastDFS tracker 节点 |
| `tc_fcgi_tracker2` | `tracker2` | `172.30.0.4` | 无 | FastDFS tracker 节点 |
| `tc_fcgi_storage1` | `nginx_fastdfs` | `172.30.0.5` | `80`, `443` | Web 入口，属于 `group1` |
| `tc_fcgi_storage2` | `storage2` | `172.30.0.6` | 无 | FastDFS storage，属于 `group1` |
| `tc_fcgi_storage3` | `storage3` | `172.30.0.7` | 无 | FastDFS storage，属于 `group2` |
| `tc_fcgi_storage4` | `storage4` | `172.30.0.8` | 无 | FastDFS storage，属于 `group2` |
| `tc_fcgi_app` | `fastcgi_app` | `172.30.0.9` | 无 | 业务 FastCGI 服务和 Redis |

说明：

- MySQL 当前没有暴露宿主机端口，只在 Docker 网络内提供 `3306`。
- 只有 `tc_fcgi_storage1` 对宿主机暴露 `80` 和 `443`。
- Tracker、Storage、FastCGI 端口只在 Docker 内部网络 `my_net` 中使用。

## 4. FastDFS 拓扑

```text
tracker1: 172.30.0.3:22122
tracker2: 172.30.0.4:22122

group1:
  storage1: 172.30.0.5:23000
  storage2: 172.30.0.6:23000

group2:
  storage3: 172.30.0.7:23000
  storage4: 172.30.0.8:23000
```

FastDFS 的 group 语义：

- 同一个 group 内的 storage 会互相同步文件。
- 不同 group 之间不会同步文件。
- group 用于横向扩容容量和吞吐。
- 上传文件时 tracker 先选择 group，再选择该 group 内的 storage。

当前 tracker 策略在 `docker/nginx_fastdfs/tracker.conf` 中：

```conf
store_lookup=3
store_server=0
store_path=0
```

含义：

- `store_lookup=3`：使用项目修改后的动态 group 选择策略。
- `store_server=0`：选定 group 后，group 内 storage 轮询。
- `store_path=0`：storage 内路径轮询。

如果希望 group 均匀轮询，可以把 `store_lookup` 改成：

```conf
store_lookup=0
```

## 5. 请求链路

### 5.1 前端页面

```text
Browser
  -> https://localhost/
  -> tc_fcgi_storage1 Nginx
  -> /app/front React 静态资源
```

HTTP 会被重定向到 HTTPS。当前证书是镜像构建时生成的自签名证书，浏览器首次访问可能会提示不受信任。

### 5.2 登录、注册、文件列表等 API

```text
Browser
  -> https://localhost/api/login
  -> tc_fcgi_storage1 Nginx
  -> tc_fcgi_app:10000
  -> MySQL / Redis
  -> 返回 JSON
```

### 5.3 文件上传

```text
Browser
  -> /api/upload 或 /api/chunk_merge
  -> FastCGI upload/chunk_merge
  -> /etc/fdfs/client.conf
  -> tracker1/tracker2
  -> group1 或 group2 的某个 storage
  -> 文件元数据写入 MySQL
```

### 5.4 文件下载和预览

```text
Browser
  -> https://localhost/group2/M00/00/00/xxx.png
  -> tc_fcgi_storage1 Nginx
  -> 路由分流
     - group1: 反向到本机 HTTP 80 上的 fastdfs-nginx-module
     - group2: 反向代理到 172.30.0.7 (storage3)
  -> 返回文件内容
```

当前入口分流规则以 `docker/nginx_fastdfs/nginx.conf` 为准；`mod_fastdfs.conf` 的 `group_count=2` 只表示模块支持多个 group，不代表入口会自动负载到所有 storage。

### 5.5 AI 图片描述和语义检索

```text
Browser
  -> https://localhost/api/ai
  -> tc_fcgi_storage1 Nginx
  -> tc_fcgi_app:10012
  -> 读取 MySQL 文件元数据
  -> 访问文件 URL，交给 DashScope Qwen-VL 生成图片描述
  -> 调用 DashScope text-embedding 生成向量
  -> 写入或查询 FAISS 本地向量索引
  -> 返回语义搜索结果
```

AI 功能需要前端请求携带 DashScope API Key。后端不会把 API Key 写死在镜像里，而是从请求体中读取后透传给 DashScope。

## 6. FastCGI 端口

| FastCGI 端口 | API | CGI 模块 | 源码 | 功能说明 |
| --- | --- | --- | --- | --- |
| `10000` | `/api/login` | `login` | `src_cgi/login_cgi.cpp` | 用户登录验证，签发 Token |
| `10001` | `/api/reg` | `register` | `src_cgi/reg_cgi.cpp` | 用户注册，写入用户信息 |
| `10002` | `/api/upload` | `upload` | `src_cgi/upload_cgi.cpp` | 普通上传（multipart/form-data） |
| `10003` | `/api/md5` | `md5` | `src_cgi/md5_cgi.cpp` | 秒传校验（MD5 去重） |
| `10004` | `/api/myfiles` | `myfiles` | `src_cgi/myfiles_cgi.cpp` | 用户文件列表查询（含排序/分页） |
| `10005` | `/api/dealfile` | `dealfile` | `src_cgi/dealfile_cgi.cpp` | 文件操作：删除、分享、下载计数(PV) |
| `10006` | `/api/sharefiles` | `sharefiles` | `src_cgi/sharefiles_cgi.cpp` | 共享文件广场与排行榜 |
| `10007` | `/api/dealsharefile` | `dealsharefile` | `src_cgi/dealsharefile_cgi.cpp` | 共享文件操作：取消分享、转存、PV |
| `10008` | `/api/sharepic` | `sharepicture` | `src_cgi/sharepicture_cgi.cpp` | 图床分享：生成/撤销分享、访问统计 |
| `10009` | `/api/chunk_init` | `chunk_init` | `src_cgi/chunk_init_cgi.cpp` | 分片上传初始化、断点信息准备 |
| `10010` | `/api/chunk_upload` | `chunk_upload` | `src_cgi/chunk_upload_cgi.cpp` | 分片数据上传 |
| `10011` | `/api/chunk_merge` | `chunk_merge` | `src_cgi/chunk_merge_cgi.cpp` | 分片合并与入库 |
| `10012` | `/api/ai` | `ai` | `src_cgi/ai_cgi.cpp` | AI 语义检索与图片描述 |

## 7. 项目目录

```text
.
├── README.md                           # 主技术文档
├── Makefile                            # 编译规则，定义所有 CGI 目标
├── ai_search.md                        # AI 检索功能说明
├── chunked_upload.md                   # 分片上传功能说明
├── common/                             # 公共模块
│   ├── base64.cpp                      # Base64 编解码
│   ├── cJSON.c                         # JSON 解析库（第三方）
│   ├── cfg.cpp                         # 配置文件解析
│   ├── dashscope_api.cpp               # DashScope API 封装
│   ├── deal_mysql.cpp                  # MySQL 操作封装
│   ├── des.cpp                         # DES 加解密
│   ├── faiss_wrapper.cpp               # FAISS 向量索引封装
│   ├── make_log.cpp                    # 日志模块
│   ├── md5.cpp                         # MD5 算法实现
│   ├── redis_op.cpp                    # Redis 操作封装
│   └── util_cgi.cpp                    # CGI 通用工具函数
├── conf/                               # 本地配置文件
│   ├── cfg.json                        # 项目总配置（MySQL/Redis/FastDFS/AI）
│   ├── client.conf                     # FastDFS Client 配置
│   ├── redis.conf                      # Redis 配置
│   ├── storage.conf                    # FastDFS Storage 配置
│   └── tracker.conf                    # FastDFS Tracker 配置
├── deps/                               # 预下载依赖源码包
│   ├── faiss-v1.7.2.tar.gz
│   ├── fastdfs-nginx-module-V1.22.tar.gz
│   ├── fastdfs-V6.06.tar.gz
│   ├── hiredis-v1.0.2.tar.gz
│   └── libfastcommon-V1.0.43.tar.gz
├── docker/                             # Docker 构建与编排
│   ├── configure_mirror.sh             # 配置镜像源
│   ├── docker-compose.yaml             # 容器编排定义
│   ├── fastcgi_app/
│   │   ├── cfg.json                    # FastCGI 容器内配置
│   │   ├── dockerfile                  # FastCGI 应用镜像构建
│   │   └── start.sh                    # FastCGI 启动脚本
│   ├── mysql/
│   │   ├── dockerfile                  # MySQL 镜像构建
│   │   └── init.sql                    # 数据库初始化脚本
│   ├── nginx_fastdfs/
│   │   ├── client.conf                 # FastDFS 客户端配置
│   │   ├── dockerfile                  # Nginx + FastDFS 镜像构建
│   │   ├── mod_fastdfs.conf            # fastdfs-nginx-module 配置
│   │   ├── nginx.conf                  # Nginx 路由与 HTTPS 配置
│   │   ├── start.sh                    # Nginx/FastDFS 启动脚本
│   │   ├── storage.conf                # FastDFS Storage 配置
│   │   └── tracker.conf                # FastDFS Tracker 配置
│   └── setup.sh                        # 项目初始化脚本
├── include/                            # 公共头文件
│   ├── base64.h
│   ├── cJSON.h
│   ├── cfg.h
│   ├── dashscope_api.h
│   ├── deal_mysql.h
│   ├── des.h
│   ├── faiss_wrapper.h
│   ├── fdfs_api.h
│   ├── make_log.h
│   ├── md5.h
│   ├── redis_keys.h
│   ├── redis_op.h
│   └── util_cgi.h
├── picture_bed/                        # React 前端项目
│   ├── package.json                    # 前端依赖定义
│   ├── package-lock.json               # 前端依赖锁文件
│   ├── public/                         # 静态资源
│   └── src/
│       ├── App.js                      # 前端主入口与路由
│       ├── components/                 # 通用组件
│       ├── config/                     # 前端配置
│       ├── contexts/                   # 全局状态上下文
│       ├── pages/                      # 页面模块
│       └── services/                   # API 请求封装
├── src_cgi/                            # CGI 业务源码（C++）
│   ├── ai_cgi.cpp                      # AI 检索接口
│   ├── chunk_init_cgi.cpp              # 分片初始化
│   ├── chunk_merge_cgi.cpp             # 分片合并
│   ├── chunk_upload_cgi.cpp            # 分片上传
│   ├── dealfile_cgi.cpp                # 文件删除/分享/PV
│   ├── dealsharefile_cgi.cpp           # 共享文件操作
│   ├── login_cgi.cpp                   # 登录接口
│   ├── md5_cgi.cpp                     # 秒传校验
│   ├── myfiles_cgi.cpp                 # 用户文件列表
│   ├── reg_cgi.cpp                     # 注册接口
│   ├── sharefiles_cgi.cpp              # 共享文件列表
│   ├── sharepicture_cgi.cpp            # 图床接口
│   └── upload_cgi.cpp                  # 普通上传接口
└── third_party/                        # 第三方源码（构建镜像时编译）
    ├── fastdfs-6.06/
    ├── fastdfs-nginx-module-1.22/
    └── libfastcommon-1.0.43/
```

重点目录：

- `src_cgi/`：业务 CGI 源码。
- `common/`：公共工具、MySQL、Redis、DashScope、FAISS 封装。
- `picture_bed/`：React 前端。
- `docker/`：Docker Compose 和镜像构建文件。
- `third_party/`：当前参与镜像构建的 FastDFS、libfastcommon、fastdfs-nginx-module 源码。

## 8. 第三方依赖

项目的第三方依赖分为两类：一类是保留在 `deps/` 中的源码压缩包，一类是已经解压到 `third_party/` 中、会被 Dockerfile 直接复制并编译的源码目录。

### 8.1 deps 目录

| 文件 | 版本 | 用途 | 使用位置 |
| --- | --- | --- | --- |
| `deps/hiredis-v1.0.2.tar.gz` | 1.0.2 | Redis C 客户端库 | `docker/fastcgi_app/dockerfile` |
| `deps/faiss-v1.7.2.tar.gz` | 1.7.2 | 向量检索库，用于 AI 语义搜索 | `docker/fastcgi_app/dockerfile` |
| `deps/libfastcommon-V1.0.43.tar.gz` | 1.0.43 | FastDFS 依赖的公共 C 库原始包 | 备份源码包 |
| `deps/fastdfs-V6.06.tar.gz` | 6.06 | FastDFS 官方源码原始包 | 备份源码包 |
| `deps/fastdfs-nginx-module-V1.22.tar.gz` | 1.22 | Nginx 访问 FastDFS 文件的模块原始包 | 备份源码包 |

### 8.2 third_party 目录

| 目录 | 版本 | 用途 | 使用位置 |
| --- | --- | --- | --- |
| `third_party/libfastcommon-1.0.43/` | 1.0.43 | FastDFS 基础依赖库 | `nginx_fastdfs`、`fastcgi_app` 镜像都会编译 |
| `third_party/fastdfs-6.06/` | 6.06 | FastDFS tracker、storage、client 源码 | `nginx_fastdfs`、`fastcgi_app` 镜像都会编译 |
| `third_party/fastdfs-nginx-module-1.22/` | 1.22 | Nginx FastDFS 模块源码 | `nginx_fastdfs` 镜像编译 Nginx 时加载 |

当前项目对 FastDFS 做了源码级修改，所以 Dockerfile 不再直接解压 `deps/fastdfs-V6.06.tar.gz`，而是复制 `third_party/fastdfs-6.06/` 参与编译。修改 FastDFS 源码后，只需要重新构建 Docker 镜像即可生效。

### 8.3 系统包依赖

`nginx_fastdfs` 镜像主要安装：

- `gcc`、`g++`、`make`
- `libtool`、`automake`、`autoconf`
- `libpcre3-dev`、`libssl-dev`、`zlib1g-dev`
- `openssl`、`curl`、`lsof`、`procps`

`fastcgi_app` 镜像主要安装：

- `gcc`、`g++`、`make`
- `libmysqlclient-dev`
- `libfcgi-dev`、`spawn-fcgi`
- `redis-server`
- `libcurl4-openssl-dev`
- `libopenblas-dev`、`liblapack-dev`
- `python3-pip`、`cmake`

## 9. 启动项目

在项目根目录执行：

```bash
docker compose -f docker/docker-compose.yaml up -d --build
```

查看容器状态：

```bash
docker compose -f docker/docker-compose.yaml ps
```

访问页面：

```text
https://localhost/
```

因为使用自签名证书，浏览器可能需要手动信任。

## 10. 停止和重启

停止容器：

```bash
docker compose -f docker/docker-compose.yaml down
```

重启已有容器：

```bash
docker compose -f docker/docker-compose.yaml restart
```

修改 Dockerfile、FastDFS 源码、前端代码或 C/C++ 业务源码后，通常需要重新构建：

```bash
docker compose -f docker/docker-compose.yaml up -d --build
```

如果只修改了挂载的 `docker/nginx_fastdfs/nginx.conf`，可以重载 Nginx：

```bash
docker exec tc_fcgi_storage1 /usr/local/nginx/sbin/nginx -s reload
```

## 11. 当前 FastDFS 源码修改点

项目已经把 FastDFS 源码放入 `third_party/fastdfs-6.06`，镜像构建时会直接复制并编译这些源码。

当前主要修改是 tracker 的 group 选择策略：

- 新增 `store_lookup=3`。
- 动态选择 group。
- group 内 storage 仍然保持轮询。
- storage 心跳上报 CPU 和内存占用。
- tracker 根据磁盘、连接、CPU、内存等指标计算 group 负载分。

相关文件：

| 文件 | 作用 |
| --- | --- |
| `third_party/fastdfs-6.06/tracker/tracker_types.h` | 新增策略枚举和 CPU/内存统计字段 |
| `third_party/fastdfs-6.06/tracker/tracker_func.c` | 允许读取 `store_lookup=3` |
| `third_party/fastdfs-6.06/tracker/tracker_service.c` | 动态 group 选择逻辑 |
| `third_party/fastdfs-6.06/storage/tracker_client_thread.c` | storage 心跳上报 CPU/内存 |
| `third_party/fastdfs-6.06/client/tracker_client.c` | 客户端解析新增统计字段 |
| `docker/nginx_fastdfs/tracker.conf` | 启用 `store_lookup=3` |

注意：`store_lookup=3` 是当前项目自定义扩展，不是 FastDFS 官方默认策略。


## 12. 常用检查命令

查看容器：

```bash
docker compose -f docker/docker-compose.yaml ps
```

查看 FastDFS 集群状态：

```bash
docker exec tc_fcgi_app fdfs_monitor /etc/fdfs/client.conf
```

查看 tracker 配置：

```bash
docker exec tc_fcgi_tracker1 grep -n "store_lookup\|store_server" /etc/fdfs/tracker.conf
```

查看 Nginx FastDFS 模块是否读到 tracker 和 group：

```bash
docker exec tc_fcgi_storage1 tail -n 120 /usr/local/nginx/logs/error.log
```

正常情况下应该能看到类似：

```text
tracker_server_count=2
group_count=2
group 1. group_name=group1
group 2. group_name=group2
```

测试文件访问：

```bash
docker exec tc_fcgi_storage1 curl -sk -o /dev/null -w '%{http_code} %{content_type} %{size_download}\n' https://127.0.0.1/group2/M00/00/00/example.png
```

查看应用日志：

```bash
docker logs --tail 160 tc_fcgi_app
docker logs --tail 160 tc_fcgi_storage1
```

## 13. 数据持久化

| 数据 | 持久化方式 | Docker Volume |
|------|-----------|---------------|
| MySQL 数据库 | Docker 命名卷 | `mysql_data:/var/lib/mysql` |
| FastDFS 文件 | Docker 命名卷 | `fastdfs_data:/fastdfs_data_and_log` |
| FAISS 索引 | 容器内磁盘 `/data/faiss/` + MySQL 备份 | 容器重建需 rebuild |
| Redis 缓存 | 仅内存（Token 等临时数据） | 容器重启后丢失 |
| 运行日志 | 容器内 `/app/logs/` | 容器重建后丢失 |

## 14. API 接口汇总

### 14.1 认证相关

| 接口 | 方法 | 说明 |
|------|------|------|
| `/api/login` | POST | 用户登录，返回 Token |
| `/api/reg` | POST | 用户注册 |

### 14.2 文件管理

| 接口 | 方法 | 参数 | 说明 |
|------|------|------|------|
| `/api/upload` | POST | multipart/form-data | 普通文件上传 |
| `/api/md5` | POST | JSON | 文件秒传检测，命中时直接复用已有文件 |
| `/api/myfiles?cmd=normal` | POST | JSON | 获取用户文件列表 |
| `/api/dealfile?cmd=del` | POST | JSON | 删除文件 |
| `/api/dealfile?cmd=share` | POST | JSON | 分享文件 |
| `/api/dealfile?cmd=pv` | POST | JSON | 更新下载计数 |

### 14.3 分片上传

| 接口 | 方法 | 说明 |
|------|------|------|
| `/api/chunk_init` | POST | 初始化分片上传，返回 `uploadedChunks` 已上传分片列表 |
| `/api/chunk_upload?md5=xxx&index=N` | POST | 上传单个分片（body=二进制数据） |
| `/api/chunk_merge` | POST | 合并所有分片 |

### 14.4 共享文件

| 接口 | 方法 | 参数 | 说明 |
|------|------|------|------|
| `/api/sharefiles?cmd=normal` | POST | JSON | 共享文件列表 |
| `/api/sharefiles?cmd=pvdesc` | POST | JSON | 下载量排行 |
| `/api/dealsharefile?cmd=save` | POST | JSON | 转存文件 |
| `/api/dealsharefile?cmd=cancel` | POST | JSON | 取消分享 |
| `/api/dealsharefile?cmd=pv` | POST | JSON | 更新下载计数 |
| `/api/sharepic` | POST | JSON | 图床分享 |

### 14.5 AI 智能检索

| 接口 | 方法 | 说明 |
|------|------|------|
| `/api/ai?cmd=describe` | POST | 生成文件 AI 描述 + 向量 |
| `/api/ai?cmd=search` | POST | 语义搜索 |
| `/api/ai?cmd=rebuild` | POST | 重建 FAISS 索引 |
| `/api/ai?cmd=get_apikey` | POST | 获取用户 API Key |
| `/api/ai?cmd=set_apikey` | POST | 保存用户 API Key |