# Docker FastDFS Cluster Architecture

本文档说明当前 Docker Compose 模拟的 FastDFS 集群架构。当前环境共有 8 个容器：1 个 MySQL、2 个 Tracker、4 个 Storage、1 个 FastCGI 应用容器。

## 容器列表

| 容器名 | Compose 服务 | IP | 对外端口 | 主要作用 |
| --- | --- | --- | --- | --- |
| `tc_fcgi_mysql` | `tc_mysql` | `172.30.0.2` | `3307 -> 3306` | 保存用户、文件元数据、分享记录等业务数据 |
| `tc_fcgi_tracker1` | `tracker1` | `172.30.0.3` | 无 | FastDFS tracker 节点，负责 storage 调度和状态维护 |
| `tc_fcgi_tracker2` | `tracker2` | `172.30.0.4` | 无 | 第二个 FastDFS tracker 节点，提高 tracker 可用性 |
| `tc_fcgi_storage1` | `nginx_fastdfs` | `172.30.0.5` | `80 -> 80`, `443 -> 443` | storage 节点，属于 `group1`；同时作为 Web/Nginx 入口 |
| `tc_fcgi_storage2` | `storage2` | `172.30.0.6` | 无 | storage 节点，属于 `group1`，和 storage1 做同组文件同步 |
| `tc_fcgi_storage3` | `storage3` | `172.30.0.7` | 无 | storage 节点，属于 `group2` |
| `tc_fcgi_storage4` | `storage4` | `172.30.0.8` | 无 | storage 节点，属于 `group2`，和 storage3 做同组文件同步 |
| `tc_fcgi_app` | `fastcgi_app` | `172.30.0.9` | 无 | 运行业务 FastCGI 程序，处理登录、注册、上传、文件列表、分享、AI 检索等接口 |

## FastDFS 拓扑

```text
FastDFS Tracker Cluster

  tracker1: 172.30.0.3:22122
  tracker2: 172.30.0.4:22122

Storage Group

  group1:
    storage1: 172.30.0.5:23000
    storage2: 172.30.0.6:23000

  group2:
    storage3: 172.30.0.7:23000
    storage4: 172.30.0.8:23000
```

说明：

- Tracker 不保存文件内容，主要负责记录 storage 状态并为上传、下载分配节点。
- 同一个 group 内的 storage 会同步文件，例如 `storage1` 和 `storage2` 都属于 `group1`。
- 不同 group 之间不互相同步，`group2` 用于横向扩容容量和吞吐。
- 当前业务配置把文件访问地址指向 `172.30.0.5:80`，也就是 `storage1` 上的 Nginx。

## 系统访问架构

```text
Browser
  |
  | HTTP/HTTPS
  v
storage1 / nginx_fastdfs
172.30.0.5:80 / 443
  |
  | /api/* FastCGI
  v
fastcgi_app
172.30.0.9:10000-10012
  |
  +--------------------+
  |                    |
  v                    v
MySQL              FastDFS Client
172.30.0.2:3306    /etc/fdfs/client.conf
                       |
                       v
              tracker1 / tracker2
              172.30.0.3:22122
              172.30.0.4:22122
                       |
                       v
           group1 / group2 storage nodes
```

## 请求链路

### 登录 / 注册 / 文件列表

```text
Browser
  -> https://localhost/api/login
  -> storage1 Nginx
  -> fastcgi_app:10000
  -> MySQL
  -> 返回 JSON
```

常见 FastCGI 端口：

| 接口 | FastCGI 端口 |
| --- | --- |
| `/api/login` | `10000` |
| `/api/reg` | `10001` |
| `/api/upload` | `10002` |
| `/api/md5` | `10003` |
| `/api/myfiles` | `10004` |
| `/api/dealfile` | `10005` |
| `/api/sharefiles` | `10006` |
| `/api/dealsharefile` | `10007` |
| `/api/sharepic` | `10008` |
| `/api/chunk_init` | `10009` |
| `/api/chunk_upload` | `10010` |
| `/api/chunk_merge` | `10011` |
| `/api/ai` | `10012` |

### 文件上传

```text
Browser
  -> storage1 Nginx
  -> fastcgi_app upload/chunk_merge
  -> FastDFS client
  -> tracker1/tracker2 查询可用 storage
  -> 写入 group1 或 group2 的某个 storage
  -> 文件元数据写入 MySQL
```

### 文件下载 / 预览

```text
Browser
  -> http://172.30.0.5/groupX/M00/...
  -> storage1 Nginx
  -> fastdfs-nginx-module
  -> 本机读取或代理到真实 storage
  -> 返回文件内容
```

## 当前端口暴露策略

当前只有两个服务暴露到宿主机：

- `tc_fcgi_storage1`: `80`, `443`
- `tc_fcgi_mysql`: `3307`

其他 tracker、storage、FastCGI 端口只在 Docker 内网 `my_net` 中使用。这样可以避免宿主机端口冲突，也更接近生产环境中“内部服务不直接暴露”的部署方式。

## 数据卷

| 数据卷 | 挂载容器 | 用途 |
| --- | --- | --- |
| `mysql_data` | `tc_fcgi_mysql` | MySQL 数据 |
| `tracker1_data` | `tc_fcgi_tracker1` | tracker1 日志和状态数据 |
| `tracker2_data` | `tc_fcgi_tracker2` | tracker2 日志和状态数据 |
| `storage1_data` | `tc_fcgi_storage1` | storage1 文件和日志 |
| `storage2_data` | `tc_fcgi_storage2` | storage2 文件和日志 |
| `storage3_data` | `tc_fcgi_storage3` | storage3 文件和日志 |
| `storage4_data` | `tc_fcgi_storage4` | storage4 文件和日志 |

## 常用检查命令

```bash
docker compose -f docker/docker-compose.yaml ps
docker exec tc_fcgi_app fdfs_monitor /etc/fdfs/client.conf
docker exec tc_fcgi_storage1 tail -n 80 /usr/local/nginx/logs/error.log
docker exec tc_fcgi_storage1 grep -n "fastcgi_pass" /usr/local/nginx/conf/nginx.conf
```
