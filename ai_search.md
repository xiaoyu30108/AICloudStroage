# AI 搜索功能说明

## 1. 功能入口

AI 搜索相关功能统一走：

```text
/api/ai?cmd=<命令>
```

当前实际支持的命令有：

- `describe`
- `search`
- `rebuild`

对应后端程序：

- `src_cgi/ai_cgi.cpp`

Nginx 路由：

- `/api/ai` -> `172.30.0.4:10012`

FastCGI 启动：

- `docker/fastcgi_app/start.sh`
- `spawn-fcgi -a 0.0.0.0 -p 10012 -f /app/bin_cgi/ai`

---

## 2. 功能依赖

AI 搜索功能主要依赖以下几部分：

### 2.1 MySQL

主要使用的表有：

- `file_info`
- `user_file_list`
- `file_ai_desc`
- `user_file_ai_desc`

### 2.2 DashScope

主要调用两个能力：

- 图片描述
- 文本 embedding

### 2.3 FAISS

用于保存和查询向量索引。

当前是每个用户一份独立索引文件，不是全局共用一份索引。

---

## 3. 数据结构

### 3.1 `file_info`

保存文件的基础信息，AI 侧主要会用到：

- `md5`
- `url`
- `size`
- `type`

### 3.2 `user_file_list`

保存用户和文件的对应关系，AI 侧主要会用到：

- `user`
- `md5`
- `file_name`

### 3.3 `file_ai_desc`

这是按 `md5` 保存的全局 AI 缓存表，主要字段有：

- `md5`
- `description`
- `embedding`
- `model`
- `status`

这张表的作用是：

- 相同 `md5` 的文件只要已经做过一次 AI 描述和 embedding，就可以复用

### 3.4 `user_file_ai_desc`

这是按 `(user, md5)` 保存的用户侧 AI 检索表，主要字段有：

- `user`
- `md5`
- `cache_id`
- `description`
- `embedding`
- `faiss_id`
- `model`
- `status`

这张表的作用是：

- 保存当前用户自己的 AI 检索数据
- 搜索时只查这张表对应当前用户的数据

---

## 4. 索引文件

### 4.1 用户索引目录

当前配置里有：

```json
"faiss": {
  "index_path": "/data/faiss/index.bin",
  "user_index_dir": "/data/faiss/users"
}
```

程序真正使用的是：

- `user_index_dir`

### 4.2 用户索引文件名

代码会先对用户名做一次 MD5，再生成索引文件路径：

```text
/data/faiss/users/<md5(user)>.index.bin
```

### 4.3 锁文件

为了避免同一个用户的索引在搜索和重建时同时读写，代码还会创建用户级锁文件：

```text
/tmp/faiss_locks/<md5(user)>.lock
```

### 4.4 脏标记文件

当用户删除文件后，如果该文件之前已经进入过 AI 检索表，代码会给当前用户写一个索引脏标记文件：

```text
/tmp/faiss_locks/<md5(user)>.dirty
```

这个标记表示：

- 当前用户索引需要在下次搜索或手动重建前做一次本地修复
- 修复过程只涉及数据库和本地 FAISS，不会重新调用 AI，也不会消耗 token

---

## 5. API Key 使用方式

只接受请求体中的 `api_key`。

也就是说：

- 前端把用户填写的 API Key 保存在浏览器本地
- 调用 `describe` 或 `search` 时，再把 `api_key` 放进请求体
- 如果请求体里没有 `api_key`，后端直接返回：

```json
{"code":1,"msg":"missing api_key"}
```

---

## 6. `describe` 逻辑

`describe` 的作用是：

- 为某个文件生成 AI 描述
- 为描述生成 embedding
- 把结果写入数据库
- 维护当前用户自己的索引

### 6.1 请求格式

```json
{
  "user": "xxx",
  "token": "xxx",
  "md5": "xxx",
  "filename": "demo.png",
  "type": "png",
  "api_key": "sk-xxx",
  "force": false,
  "skip_rebuild": false
}
```

其中：

- `user`、`token`、`md5`、`filename` 必填
- `type` 可选
- `api_key` 必须有
- `force` 可选
- `skip_rebuild` 可选，主要用于批量强制重建时暂时跳过立即重建

### 6.2 处理流程

当前代码流程如下：

1. 解析 JSON
2. 检查请求字段是否完整
3. 校验用户 `token`
4. 校验请求体中是否携带 `api_key`
5. 连接 MySQL
6. 在 `user_file_list` 中检查当前用户是否拥有该 `md5`
7. 如果当前用户在 `user_file_ai_desc` 中已经有 `status=1` 的记录，并且不是强制更新，直接返回 `already exists`
8. 如果不是强制更新，则先查 `file_ai_desc`
9. 如果 `file_ai_desc` 中已经有可复用的完成记录，就把这份缓存复制到 `user_file_ai_desc`
10. 复制成功后，优先把该条向量直接追加到当前用户索引；如果追加失败，再回退到整份重建
11. 如果不能复用缓存，则重新生成文件描述
12. 再生成 embedding
13. 把结果写入 `file_ai_desc`
14. 再写入 `user_file_ai_desc`
15. 维护当前用户索引
16. 如果是普通 `describe`，优先单条追加到当前用户索引；失败时再整份重建
17. 如果是 `force=true` 且 `skip_rebuild=false`，立即整份重建当前用户索引
18. 如果是 `force=true` 且 `skip_rebuild=true`，本次只更新数据库，不立即重建
19. 返回成功

### 6.3 批量重建时的实际调用方式

前端在 `Home.js` 中做“重建 AI 描述”时，当前实际逻辑是：

1. 先拉取当前用户文件列表
2. 逐个文件调用 `describeFileByMd5(..., force=true, skip_rebuild=true)`
3. 循环结束后，再额外调用一次 `/api/ai?cmd=rebuild`

这样做的作用是：

- 避免每个文件都触发一次整份索引重建
- 把批量重建链路收敛为“逐个 describe + 最后一次 rebuild”

### 6.4 文件描述生成方式

#### 图片文件

如果文件类型属于图片：

- 先从 `file_info` 表查询文件 URL
- 再拼成可访问的图片地址
- 调用图片描述接口生成描述

如果图片描述失败，则降级为：

```text
图片文件：<filename>
```

#### 文本 / docx 文件

如果文件类型是：

- `docx`
- 或文本类文件：`txt/md/csv/json/xml/html/htm/log/c/cpp/h/py/js/css/java`

则会：

1. 根据 `file_info.url` 下载文件到 `/tmp`
2. 如果是 `docx`，从 `word/document.xml` 中提取文本
3. 如果是普通文本文件，直接读取内容
4. 截取前 3000 个字符作为描述内容

描述格式大致是：

```text
文件名：xxx
文件内容：...
```

#### 其他类型

如果不是图片，也不是可提取文本的类型，则描述会退化成：

```text
<type>类型的文件：<filename>
```

### 6.5 embedding 生成

描述文本生成后，会调用 embedding 接口生成向量。

生成成功后，代码会对向量做 L2 归一化。

---

## 7. `search` 逻辑

`search` 的作用是：

- 根据自然语言查询当前用户自己的文件

### 7.1 请求格式

```json
{
  "user": "xxx",
  "token": "xxx",
  "query": "红色沙发上的猫",
  "api_key": "sk-xxx"
}
```

### 7.2 处理流程

当前代码流程如下：

1. 解析 JSON
2. 检查请求字段
3. 校验 `token`
4. 校验 `query` 是否为空
5. 校验请求体里是否带了 `api_key`
6. 连接 MySQL
7. 如果当前用户存在索引脏标记，则先本地重建一次该用户索引，并清掉脏标记
8. 调用 `sync_missing_user_ai_from_cache(conn, user)`
9. 如果这一步同步出了新记录，则优先把尚未进入索引的记录增量补进当前用户索引
10. 如果增量补录失败，再回退到 `rebuild_user_faiss_index(conn, user)`
11. 对查询文本生成 embedding
12. 对查询向量做 L2 归一化
13. 根据用户名计算当前用户索引文件路径
14. 给该用户索引加读锁
15. 加载当前用户自己的 FAISS 索引
16. 如果当前索引为空，但数据库中该用户已经有可用向量，则自动重建一次索引
17. 调用 FAISS 搜索 TopK
18. 按阈值过滤结果
19. 根据 `faiss_id` 回查当前用户自己的文件信息
20. 返回结果 JSON

### 7.3 搜索范围

当前搜索只查当前用户自己的数据。

回查结果时使用的是：

- `user_file_ai_desc`
- `user_file_list`
- `file_info`

并且 SQL 里带有：

```sql
WHERE uad.user = '<当前用户>'
```

所以当前逻辑不会把其他用户的文件直接作为搜索结果返回。

### 7.4 搜索参数

当前代码里固定写死：

- `topk = 10`
- `score_threshold = 0.45`

即：

- 最多返回 10 个候选
- 分数低于 `0.45` 的结果会被丢弃

### 7.5 返回字段

搜索结果里会返回：

- `md5`
- `filename`
- `description`
- `url`
- `size`
- `type`
- `score`

---

## 8. `rebuild` 逻辑

`rebuild` 的作用是：

- 重建当前用户自己的 FAISS 索引

### 8.1 请求格式

```json
{
  "user": "xxx",
  "token": "xxx"
}
```

### 8.2 处理流程

当前代码流程如下：

1. 解析 JSON
2. 检查请求字段
3. 校验 `token`
4. 连接 MySQL
5. 调用 `sync_missing_user_ai_from_cache(conn, user)`
6. 调用 `rebuild_user_faiss_index(conn, user)`
7. 清掉当前用户的索引脏标记
8. 返回重建数量

---

## 9. 缓存同步逻辑

### 9.1 `sync_missing_user_ai_from_cache`

这个函数的作用是：

- 如果当前用户在 `user_file_list` 中拥有某个文件
- 且这个文件在 `file_ai_desc` 中已经有完成的 AI 缓存
- 但 `user_file_ai_desc` 里还没有这条用户记录
- 就自动补进 `user_file_ai_desc`

这样做之后：

- 其他用户已经做过 AI 的相同文件
- 当前用户可以直接复用
- 不需要重新调用 AI

### 9.2 `append_pending_user_faiss_entries`

这个函数的作用是：

- 在 `sync_missing_user_ai_from_cache` 之后
- 把当前用户 `user_file_ai_desc` 中已经有 embedding、但 `faiss_id < 0` 的记录
- 批量补进当前用户自己的 FAISS 索引

这一步的意义是：

- `search` 时不必因为同步到几条新记录就立刻整份重建索引
- 可以优先做增量补录
- 只有增量补录失败时，才回退到整份重建

---

## 10. 用户索引重建逻辑

### 10.1 `rebuild_user_faiss_index`

这个函数会重建某个用户自己的索引。

执行步骤如下：

1. 确保用户索引目录存在
2. 获取该用户写锁
3. 计算该用户索引文件路径
4. 调用 `faiss_reset()`
5. 删除旧索引文件
6. 调用 `faiss_init()` 创建新索引
7. 调用 `faiss_set_auto_save(0)` 暂时关闭每次 add 后自动保存
8. 先把当前用户 `user_file_ai_desc` 中所有记录的 `faiss_id` 重置为 `-1`
9. 查询当前用户所有 `status=1` 且 embedding 非空的记录
10. 把每条 embedding 加入 FAISS
11. 把返回的 `faiss_id` 回写到 `user_file_ai_desc`
12. 调用 `faiss_set_auto_save(1)`
13. 调用 `faiss_save(index_path)` 把整份索引保存到磁盘
14. 释放锁

### 10.2 单条追加与增量补录

除了整份 `rebuild_user_faiss_index` 外，当前代码还存在两种更轻量的索引维护方式：

- `append_user_faiss_entry`
- `append_pending_user_faiss_entries`

其中：

- `append_user_faiss_entry` 用于普通 `describe` 成功后，把单个文件向量追加到当前用户索引
- `append_pending_user_faiss_entries` 用于 `search` 前缓存同步之后，把当前用户尚未入索引的多条记录批量补进索引

当前整体策略是：

- 能单条追加就不整份重建
- 能增量补录就不整份重建
- 只有失败或明确要求时才走整份 `rebuild`

### 10.3 删除文件后的索引修复

当前删除文件时，`dealfile_cgi.c` 会：

1. 从 `user_file_list` 删除用户文件记录
2. 从 `user_file_ai_desc` 删除该用户对应的 AI 检索记录
3. 给当前用户写入索引脏标记

之后：

- 下次执行 `search` 时，AI 后端会先检查脏标记
- 如果标记存在，就先本地重建当前用户索引
- 重建成功后清掉脏标记，再继续搜索

这样做的作用是：

- 避免删除文件后继续残留“幽灵向量”
- 修复过程只在服务器本地完成
- 不会重新调用 AI，也不会消耗 token

---

## 11. FAISS 封装逻辑

对应代码：

- `include/faiss_wrapper.h`
- `common/faiss_wrapper.cpp`

### 11.1 索引类型

当前使用的是：

- `faiss::IndexFlatIP`

### 11.2 `faiss_init`

作用：

- 如果目标索引文件已经存在，就加载
- 如果不存在，就创建空索引
- 如果当前进程已经加载了别的路径索引，就切换到新路径

### 11.3 `faiss_add`

作用：

- 把向量加入索引
- 返回本次向量的 `faiss_id`

### 11.4 `faiss_search`

作用：

- 在当前索引中进行 TopK 搜索

### 11.5 `faiss_save`

作用：

- 把当前内存索引保存到磁盘文件

### 11.6 `faiss_reset`

作用：

- 释放当前索引对象
- 清空当前索引状态

---

## 12. 一条完整流程示例

### 12.1 用户上传新文件后调用 `describe`

流程如下：

1. 前端拿到文件的 `md5`
2. 调用 `/api/ai?cmd=describe`
3. 后端检查当前用户是否拥有该文件
4. 先尝试复用 `file_ai_desc`
5. 如果不能复用，则重新生成描述和向量
6. 写入数据库
7. 普通场景优先单条追加到当前用户索引，失败时再整份重建

### 12.2 用户调用 `search`

流程如下：

1. 前端把搜索词和 `api_key` 发给 `/api/ai?cmd=search`
2. 后端先检查当前用户索引是否带有删除后的脏标记
3. 如果存在脏标记，先本地重建一次当前用户索引
4. 再同步当前用户可以复用的缓存记录
5. 如果同步到了新记录，优先做增量补索引
6. 再对查询文本生成向量
7. 在当前用户自己的索引中搜索
8. 返回当前用户自己的文件结果

### 12.3 用户调用 `rebuild`

流程如下：

1. 前端调用 `/api/ai?cmd=rebuild`
2. 后端同步缓存
3. 重建当前用户自己的 FAISS 索引
4. 清除索引脏标记
5. 返回重建数量

### 12.4 用户删除文件

流程如下：

1. 删除接口先删除 `user_file_list` 中该用户的文件记录
2. 再删除 `user_file_ai_desc` 中该用户对应的 AI 检索记录
3. 如果确实删到了 AI 检索记录，就写入当前用户索引脏标记
4. 下一次 `search` 或手动 `rebuild` 时，再把该用户索引修复到最新状态

---

## 13. 相关文件

AI 后端主逻辑：

- `src_cgi/ai_cgi.cpp`

FAISS 封装：

- `include/faiss_wrapper.h`
- `common/faiss_wrapper.cpp`

Nginx 路由：

- `docker/nginx_fastdfs/nginx.conf`

FastCGI 启动：

- `docker/fastcgi_app/start.sh`

数据库初始化：

- `docker/mysql/init.sql`

---

## 14. 功能亮点

### 14.1 相同文件可复用全局 AI 描述

当前代码里，`file_ai_desc` 是按 `md5` 保存的全局缓存表。

这意味着：

- 如果某个文件已经做过一次 AI 描述和 embedding
- 其他用户拥有相同 `md5` 的文件时
- 可以直接复用这份描述和向量

这样可以减少重复调用 AI。

### 14.2 搜索范围只限当前用户自己的文件

当前 `search` 不是先查全局结果再做兜底过滤，而是：

- 只加载当前用户自己的 FAISS 索引
- 只查询当前用户自己的 `user_file_ai_desc`
- 只结合当前用户自己的 `user_file_list`

所以搜索结果范围和用户文件范围是一致的。

### 14.3 搜索前会自动同步可复用缓存

在执行 `search` 和 `rebuild` 前，代码会先调用：

- `sync_missing_user_ai_from_cache`

如果当前用户拥有某个文件，而该文件已经在 `file_ai_desc` 中有完成记录，就会自动同步到当前用户的 `user_file_ai_desc`。

这样即使当前用户自己还没单独做过 `describe`，也可以复用已有缓存。

### 14.4 搜索链路支持增量补索引

当前 `search` 不再是“同步到新记录后就直接整份 rebuild”，而是：

- 先同步缓存
- 再尝试把未入索引记录增量补进 FAISS
- 只有失败时才回退到整份 rebuild

这样可以减少首次搜索或缓存同步场景下的阻塞时间。

### 14.5 每个用户独立索引，重建粒度更小

当前不是所有用户共用一个索引，而是每个用户一份索引文件。

这样做的直接效果是：

- 重建时只重建当前用户自己的索引
- 不会影响其他用户的索引数据
- 排查问题时也更容易定位到具体用户

### 14.6 删除文件后会延迟修复用户索引

当前删除文件时，不会去重新调用 AI，而是：

- 先删掉用户自己的 AI 检索记录
- 再给当前用户打索引脏标记
- 在下次 `search` 或手动 `rebuild` 时完成本地索引修复

这样可以把“删除文件后的幽灵向量”问题收掉，同时不增加 token 消耗。

### 14.7 文本类文件会尽量提取真实内容

对于可识别的文本类文件，当前不是只拿文件名做描述，而是会尽量提取文件内容：

- `docx` 提取正文文本
- `txt/md/json/cpp/py/js` 等文本类文件直接读取内容

这样生成出来的描述更接近文件真实内容，搜索命中通常也会更准确。
