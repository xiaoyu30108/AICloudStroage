# AI 智能检索功能 — 技术实现文档

## 1. 功能概述

AI 智能检索功能为云存储系统提供**基于语义的文件搜索能力**。用户上传文件后，系统自动调用阿里百炼（DashScope）AI 服务生成文件内容描述，并将描述转化为高维向量存入 FAISS 索引。搜索时，将用户的自然语言查询转化为向量，通过余弦相似度匹配找到语义最相关的文件。

### 核心能力

| 功能 | 说明 |
|------|------|
| 图片智能描述 | 调用 Qwen-VL 多模态模型，自动识别图片内容并生成中文描述 |
| 文档内容提取 | 支持 txt/md/csv/json/docx 等文本类文件的内容提取 |
| 语义向量检索 | 基于 FAISS 向量索引，支持自然语言搜索（如"红色沙发上的猫"） |
| API Key 管理 | 每用户独立 API Key，支持数据库持久化存储 |
| 索引重建 | 从 MySQL 持久化数据重建 FAISS 内存索引 |

---

## 2. 系统架构

```
┌─────────────────┐     HTTPS      ┌──────────────┐    FastCGI     ┌──────────────────┐
│   React 前端    │ ────────────── │    Nginx     │ ────────────── │   ai_cgi (C++)   │
│  (Home.js)      │   /api/ai?cmd= │  (port 443)  │   port 10012  │  FastCGI 进程     │
│  (ai.js)        │                │              │               │                  │
└─────────────────┘                └──────────────┘               └────────┬─────────┘
                                                                          │
                                          ┌───────────────────────────────┼──────────────────┐
                                          │                               │                  │
                                   ┌──────▼──────┐               ┌───────▼───────┐  ┌───────▼───────┐
                                   │   DashScope  │               │    MySQL      │  │    FAISS      │
                                   │   API        │               │  (172.30.0.2) │  │  内存索引      │
                                   │  (阿里百炼)   │               │               │  │  + 磁盘持久化  │
                                   └──────────────┘               └───────────────┘  └───────────────┘
```

### 容器拓扑

| 容器 | IP | 端口 | 角色 |
|------|----|------|------|
| `tc_fcgi_nginx_fastdfs` | 172.30.0.3 | 80/443 | Nginx 反向代理 + FastDFS 存储 |
| `tc_fcgi_app` | 172.30.0.4 | 10012 | AI FastCGI 后端进程 |
| `tc_fcgi_mysql` | 172.30.0.2 | 3306 | MySQL 数据库 |

---

## 3. 数据库设计

### 3.1 核心表：`file_ai_desc`

```sql
CREATE TABLE `file_ai_desc` (
  `id`          bigint NOT NULL AUTO_INCREMENT,
  `md5`         varchar(256) NOT NULL COMMENT '对应 file_info.md5',
  `description` text NOT NULL         COMMENT 'AI 生成的文件内容描述',
  `embedding`   mediumblob DEFAULT NULL COMMENT '向量序列化 float[1024]，重建索引用',
  `faiss_id`    int DEFAULT -1        COMMENT 'FAISS 索引中的 ID',
  `model`       varchar(64) DEFAULT '' COMMENT '使用的模型名',
  `status`      tinyint DEFAULT 0     COMMENT '0=待处理 1=完成 2=失败',
  `create_time` timestamp NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_md5` (`md5`(191))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

**字段说明：**

- `md5`：文件唯一标识，关联 `file_info.md5`
- `description`：AI 生成的文件描述文本。图片由 Qwen-VL 生成，文本文件直接提取内容
- `embedding`：1024 维 float 向量的二进制序列化（4096 字节），用于索引重建
- `faiss_id`：该记录在 FAISS 内存索引中的位置 ID，搜索时用于反查
- `status`：处理状态。1=完成（描述和向量均已生成），2=描述生成成功但向量化失败

### 3.2 关联表

- **`file_info`**：存储文件元数据（md5, url, size, type），`describe` 时用于获取文件下载 URL
- **`user_file_list`**：用户-文件关联表，`search` 时用于获取文件名
- **`user_info`**：用户表，包含 `api_key` 字段用于存储每用户的 DashScope API Key

---

## 4. API 接口详细说明

所有接口通过统一入口 `/api/ai` 访问，通过 URL 参数 `cmd` 区分操作类型。

Nginx 将 `/api/ai` 路由转发到 `172.30.0.4:10012`（FastCGI 协议）。

### 4.1 describe — 生成文件 AI 描述

**用途**：为已上传的文件生成 AI 描述和向量嵌入，存入数据库和 FAISS 索引。

**请求**：
```
POST /api/ai?cmd=describe
Content-Type: application/json

{
  "user": "username",
  "token": "login_token",
  "md5": "文件MD5",
  "filename": "example.png",
  "type": "png",
  "api_key": "sk-xxx",    // 可选，优先使用
  "force": true            // 可选，强制重新生成
}
```

**响应**：
```json
{ "code": 0, "msg": "ok" }
{ "code": 0, "msg": "already exists" }   // 已有描述且非强制更新
{ "code": 1, "msg": "embedding failed" } // 向量化失败
{ "code": 4, "msg": "token error" }      // Token 验证失败
```

**处理流程**：

```
┌─────────────┐
│ 解析请求参数 │
└──────┬──────┘
       ▼
┌─────────────────┐
│ 验证 Token      │ ── 失败 → 返回 code=4
└──────┬──────────┘
       ▼
┌──────────────────────────┐
│ 检查 file_ai_desc 表     │
│ 是否已有 status=1 的记录 │ ── 是且非 force → 返回 "already exists"
└──────┬───────────────────┘
       ▼
┌────────────────────────┐
│ 判断文件类型            │
│ is_image_type(type) ?  │
└──────┬────────┬────────┘
       │        │
    图片类     非图片类
       │        │
       ▼        ▼
┌──────────┐  ┌──────────────────────┐
│ 查 DB 获  │  │ 查 DB 获取文件 URL    │
│ 取文件URL │  │ 下载到 /tmp/          │
│ 构造公网  │  │ 提取文本内容           │
│ 可访问URL │  │ (docx: unzip XML解析) │
│ 调用      │  │ (txt等: 直接读取)      │
│ Qwen-VL   │  │ 截取前 3000 字符       │
└──────┬───┘  └──────────┬───────────┘
       │                 │
       ▼                 ▼
┌──────────────────────────┐
│ 得到 description 文本     │
└──────┬───────────────────┘
       ▼
┌──────────────────────────┐
│ 调用 text-embedding-v3   │
│ 获取 1024 维向量          │ ── 失败 → 存 status=2, 返回错误
└──────┬───────────────────┘
       ▼
┌──────────────────────────┐
│ L2 归一化向量             │
│ 添加到 FAISS 索引         │
│ 获得 faiss_id             │
└──────┬───────────────────┘
       ▼
┌──────────────────────────┐
│ REPLACE INTO file_ai_desc│
│ 存储: md5, description,  │
│ embedding(blob), faiss_id│
│ model, status=1          │
└──────┬───────────────────┘
       ▼
   返回 code=0
```

**图片描述生成细节**：

1. 从 `file_info` 表查询文件 URL
2. 提取路径部分（`/group1/M00/...`），使用公网 IP 构造完整 URL
   - 公网 URL 格式：`http://{public_server_ip}:{port}/group1/M00/...`
   - 这样 DashScope 云端 AI 服务才能访问到图片
3. 调用 `qwen-vl-plus` 模型，Prompt："请用中文详细描述这张图片的内容，包括主要物体、场景、颜色、文字等信息"
4. 若 AI 调用失败，降级使用文件名作为描述

**文档内容提取细节**：

| 文件类型 | 提取方式 |
|----------|----------|
| docx | 解压 ZIP → 解析 `word/document.xml` → 提取 `<w:t>` 标签内文本 |
| txt/md/csv/json/xml/html/log/c/cpp/h/py/js/css/java | 直接读取文件内容 |
| 其他类型 | 使用 "文件名 + 类型" 作为描述 |

- 文本内容截取前 3000 字符（避免 embedding API token 限制）
- 非图片文件使用容器内网地址下载（容器间互通）

### 4.2 search — 语义搜索

**用途**：将自然语言查询转化为向量，在 FAISS 索引中搜索语义最相似的文件。

**请求**：
```
POST /api/ai?cmd=search
Content-Type: application/json

{
  "user": "username",
  "token": "login_token",
  "query": "红色沙发上的猫",
  "api_key": "sk-xxx"     // 可选
}
```

**响应**：
```json
{
  "code": 0,
  "count": 3,
  "files": [
    {
      "md5": "abc123...",
      "filename": "cat_photo.jpg",
      "description": "一张照片，红色沙发上躺着一只橘色的猫...",
      "url": "http://..../group1/M00/00/00/xxx.jpg",
      "size": "1234567",
      "type": "jpg",
      "score": 0.82
    }
  ]
}
```

**处理流程**：

```
┌────────────────────┐
│ 解析请求 + 验证Token│
└──────┬─────────────┘
       ▼
┌────────────────────┐
│ 检查 FAISS 索引     │
│ ntotal == 0 ?      │ ── 是 → 返回空结果
└──────┬─────────────┘
       ▼
┌────────────────────────────┐
│ 调用 text-embedding-v3     │
│ 将查询文本转为 1024 维向量  │
└──────┬─────────────────────┘
       ▼
┌────────────────────────────┐
│ L2 归一化查询向量           │
│ IndexFlatIP 搜索 Top 10    │
└──────┬─────────────────────┘
       ▼
┌────────────────────────────┐
│ 过滤 score < 0.45 的结果   │
│ (余弦相似度阈值)            │
└──────┬─────────────────────┘
       ▼
┌────────────────────────────────────────────┐
│ 对每个匹配结果:                             │
│ 1. 通过 faiss_id 查 file_ai_desc 获取 md5  │
│ 2. JOIN file_info 获取 url/size/type        │
│ 3. 查 user_file_list 获取文件名             │
│    (优先查当前用户，其次查所有用户)            │
└──────┬─────────────────────────────────────┘
       ▼
   返回 JSON 结果
```

**关键参数**：
- Top-K：10（最多返回 10 条结果）
- 相似度阈值：0.45（余弦相似度，范围 -1 到 1）
- 低于阈值的结果会被过滤

### 4.3 rebuild — 重建 FAISS 索引

**用途**：当 FAISS 内存索引丢失（如进程重启但磁盘文件损坏）或需要修复索引时，从 MySQL 中存储的 embedding 数据重建索引。

**请求**：
```
POST /api/ai?cmd=rebuild
Content-Type: application/json

{
  "user": "username",
  "token": "login_token"
}
```

**响应**：
```json
{ "code": 0, "msg": "rebuilt", "count": 42 }
```

**处理流程**：

1. 调用 `faiss_reset()` 释放旧索引
2. 删除磁盘索引文件
3. 创建全新空索引 `faiss_init()`
4. 查询 `file_ai_desc` 中所有 `status=1` 且 `embedding IS NOT NULL` 的记录
5. 逐条验证 embedding 大小（必须等于 `1024 * sizeof(float) = 4096` 字节）
6. 添加到 FAISS 索引，获取新 `faiss_id`
7. 更新 MySQL 中对应记录的 `faiss_id`

### 4.4 get_apikey / set_apikey — API Key 管理

**get_apikey** — 获取用户的 API Key：
```
POST /api/ai?cmd=get_apikey
{ "user": "xxx", "token": "xxx" }
→ { "code": 0, "api_key": "sk-xxx" }
```

**set_apikey** — 保存用户的 API Key：
```
POST /api/ai?cmd=set_apikey
{ "user": "xxx", "token": "xxx", "api_key": "sk-xxx" }
→ { "code": 0, "msg": "ok" }
```

**API Key 解析优先级**（`resolve_api_key` 函数）：

```
1. 请求体中的 api_key 字段      ← 最高优先级
2. 数据库 user_info.api_key      ← 用户级
3. 配置文件 conf/cfg.json         ← 全局默认（通常为空）
```

---

## 5. 向量索引实现

### 5.1 FAISS 索引类型

使用 **`IndexFlatIP`**（内积索引），配合 L2 归一化实现余弦相似度搜索。

```
余弦相似度 = 内积(normalize(A), normalize(B))
```

- 索引类型：`faiss::IndexFlatIP`（暴力内积搜索，无近似）
- 向量维度：1024（text-embedding-v3 输出维度）
- 归一化：添加和搜索时均做 L2 归一化
- 相似度范围：-1 到 1（归一化后内积等价于余弦相似度）

### 5.2 持久化机制

```
内存索引 (IndexFlatIP)  ←──→  磁盘文件 (/data/faiss/index.bin)
       ↕                              ↕ (备份)
   运行时搜索             MySQL file_ai_desc.embedding (mediumblob)
```

- **每次 `faiss_add()` 后自动保存**到磁盘（`faiss::write_index`）
- **进程启动时**自动从磁盘加载索引（`faiss::read_index`）
- **MySQL embedding 字段**作为二级备份，支持 rebuild 重建

### 5.3 ID 映射

FAISS 使用自增整数 ID（`faiss_id = ntotal`，即添加前的向量总数）。

```
FAISS faiss_id → file_ai_desc.faiss_id → file_ai_desc.md5 → file_info / user_file_list
```

搜索时通过 `faiss_id` 反查 `file_ai_desc` 表，再 JOIN `file_info` 获取文件详情。

---

## 6. DashScope API 集成

### 6.1 图片描述 — Qwen-VL

- **API 端点**：`https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation`
- **模型**：`qwen-vl-plus`
- **输入**：图片 URL（必须公网可访问）+ 文本 Prompt
- **Prompt**："请用中文详细描述这张图片的内容，包括主要物体、场景、颜色、文字等信息。"
- **超时**：60 秒
- **响应解析路径**：`output.choices[0].message.content[0].text`

**请求示例**：
```json
{
  "model": "qwen-vl-plus",
  "input": {
    "messages": [{
      "role": "user",
      "content": [
        { "image": "http://xxx.xx.xx.xx:8080/group1/M00/00/00/xxx.png" },
        { "text": "请用中文详细描述这张图片的内容..." }
      ]
    }]
  }
}
```

### 6.2 文本向量化 — text-embedding-v3

- **API 端点**：`https://dashscope.aliyuncs.com/api/v1/services/embeddings/text-embedding/text-embedding`
- **模型**：`text-embedding-v3`
- **输入维度**：1024
- **超时**：30 秒
- **响应解析路径**：`output.embeddings[0].embedding[]`

**请求示例**：
```json
{
  "model": "text-embedding-v3",
  "input": { "texts": ["一张照片，红色沙发上躺着一只猫"] },
  "parameters": { "dimension": 1024 }
}
```

---

## 7. 前端实现

### 7.1 文件结构

| 文件 | 职责 |
|------|------|
| `src/services/ai.js` | AI 相关 API 调用封装 |
| `src/pages/Home.js` | 搜索界面 + 结果展示 + API Key 设置 |
| `src/config/index.js` | API 地址配置 |

### 7.2 前端 API 函数

| 函数 | 说明 |
|------|------|
| `fetchApiKey(user)` | 获取用户 API Key（数据库 → localStorage 迁移） |
| `saveApiKey(key, user)` | 保存 API Key 到数据库 |
| `describeFile(file, user, apiKey)` | 上传后触发 AI 描述生成（异步，不阻塞上传） |
| `describeFileByMd5(md5, filename, type, user, apiKey)` | 重新生成已有文件的 AI 描述（force=true） |
| `aiSearch(query, user, apiKey)` | 语义搜索 |
| `rebuildIndex(user)` | 重建 FAISS 索引 |

### 7.3 触发时机

- **文件上传完成后**：自动调用 `describeFile()`，后台异步生成描述，失败不影响上传
- **用户点击"重建AI描述"按钮**：调用 `describeFileByMd5()`，强制重新生成指定文件的描述
- **用户在搜索框输入并提交**：调用 `aiSearch()`，返回匹配文件列表
- **用户点击"重建索引"按钮**：调用 `rebuildIndex()`，从 MySQL 重建 FAISS 索引


## 8. 支持的文件类型

### 图片类（调用 Qwen-VL）

`png` `jpg` `jpeg` `gif` `bmp` `webp` `svg`

### 文本类（直接提取内容）

`txt` `md` `csv` `json` `xml` `html` `htm` `log` `c` `cpp` `h` `py` `js` `css` `java`

### 文档类（特殊解析）

`docx` — 通过 unzip 解压，解析 `word/document.xml` 中的 `<w:t>` 标签提取纯文本

### 其他类型

使用 "文件类型 + 文件名" 作为描述文本进行向量化

---

## 9. 配置项

配置文件路径：`conf/cfg.json`

```json
{
  "dashscope": {
    "api_key": "",                        // 全局默认 API Key（通常为空，由用户自行配置）
    "embedding_model": "text-embedding-v3",
    "vl_model": "qwen-vl-plus",
    "embedding_dimension": "1024"
  },
  "faiss": {
    "index_path": "/data/faiss/index.bin"
  },
  "public_server": {
    "ip": "xxx.xx.xx.xx",               // 公网 IP，用于构造图片 URL 给 DashScope
    "port": "8080"
  },
  "web_server": {
    "ip": "114.215.169.66",               // 内网/容器间访问 IP
    "port": "80"
  }
}
```

---

## 10. 错误处理与降级策略

| 场景 | 处理方式 |
|------|----------|
| Qwen-VL 图片描述失败 | 降级为 "图片文件：{filename}" 作为描述 |
| 文件下载失败 | 降级为 "{type}类型的文件：{filename}" |
| Embedding API 调用失败 | 存储 description 但标记 status=2（失败），不加入 FAISS |
| FAISS 索引为空 | search 直接返回 `{"code":0, "count":0, "files":[]}` |
| FAISS 索引文件损坏 | 创建新空索引；用户可通过 rebuild 从 MySQL 恢复 |
| Token 验证失败 | 返回 code=4，前端提示重新登录 |
| API Key 未配置 | 返回 code=1，msg="missing api_key" |
| 前端 describe 调用失败 | 不影响文件上传流程（异步非阻塞） |

---

## 11. 进程与部署

### 编译

AI CGI 模块在 Makefile 中定义，依赖 FAISS、libcurl、MySQL client 等：

```makefile
$(ai): ai_cgi.o dashscope_api.o faiss_wrapper.o ...
    $(CXX) $^ -o $@ $(LIBS) -lfaiss -lcurl
```

### 运行

使用 `spawn-fcgi` 启动，监听 10012 端口：

```bash
spawn-fcgi -a 0.0.0.0 -p 10012 -f /app/bin_cgi/ai
```

### Nginx 路由

```nginx
location /api/ai {
    fastcgi_pass 172.30.0.4:10012;
    fastcgi_read_timeout 600;    # AI 调用可能较慢
    include fastcgi.conf;
}
```

注意 `fastcgi_read_timeout` 设为 600 秒，因为 AI 模型调用（特别是图片描述）可能需要较长时间。

---

## 12. 完整数据流示例

### 上传文件后生成 AI 描述

```
用户上传 cat.jpg
    │
    ▼
前端上传到 /api/upload → FastDFS 存储 → 返回 md5 + url
    │
    ▼
前端异步调用: POST /api/ai?cmd=describe
    body: { user, token, md5, filename:"cat.jpg", type:"jpg", api_key }
    │
    ▼
后端 ai_cgi:
    1. 从 file_info 查到 url = group1/M00/00/00/xxx.jpg
    2. 构造公网 URL: http://xxx.xx.xx.xx:8080/group1/M00/00/00/xxx.jpg
    3. 调用 Qwen-VL API → "这是一张橘猫躺在红色沙发上的照片..."
    4. 调用 text-embedding-v3 → float[1024] 向量
    5. L2 归一化 → faiss_add → faiss_id=42
    6. INSERT INTO file_ai_desc (md5, description, embedding, faiss_id, status=1)
```

### 语义搜索

```
用户输入 "猫的照片"
    │
    ▼
前端调用: POST /api/ai?cmd=search
    body: { user, token, query:"猫的照片", api_key }
    │
    ▼
后端 ai_cgi:
    1. 调用 text-embedding-v3("猫的照片") → query_vec[1024]
    2. L2 归一化 query_vec
    3. FAISS IndexFlatIP.search(query_vec, top10)
       → ids=[42, 15, 7, ...], scores=[0.82, 0.61, 0.33, ...]
    4. 过滤 score < 0.45 → 保留 id=42(0.82), id=15(0.61)
    5. 查 file_ai_desc WHERE faiss_id=42 → md5=abc123
    6. JOIN file_info → url, size, type
    7. 查 user_file_list → filename="cat.jpg"
    8. 返回结果列表
```
