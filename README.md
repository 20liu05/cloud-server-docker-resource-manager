# 云服务器资源管理（docker,镜像）

一个面向云服务器的深色风格 Docker 资源管理面板。它可以查看容器、镜像、CPU、内存、磁盘，并执行常用 Docker 控制操作。

后端是单文件 C 程序，部署脚本会在服务器上编译为静态二进制，再通过 `FROM scratch` 构建容器镜像。这样即使服务器无法拉取 Node、Alpine 等基础镜像，也可以离线构建运行。

## 功能

- 监听 CPU、内存、存储、Docker 运行状态
- 查看 Docker 容器和镜像
- 启动、停止、删除容器
- 基于现有镜像创建容器
- 拉取和删除镜像
- 受保护容器机制，默认保护面板自身 `docker-panel`
- 页面内新增或移除受保护容器
- 页面内修改登录账号和密码
- 深色控制台布局，资源搜索，容器/镜像标签切换

## 部署

服务器需要已经安装：

- Docker
- Docker Compose
- GCC

```bash
chmod +x deploy.sh
./deploy.sh
```

访问：

```text
http://你的服务器IP:9849
```

部署脚本会自动生成 `.env` 和 `data/panel.conf`。默认登录信息：

- 用户名：`admin`
- 密码：`123456`

登录后可在页面内修改账号和密码。

## 维护

```bash
docker compose ps
docker compose logs -f
docker compose restart
docker compose down
```

## 配置

`.env` 控制容器端口：

```env
PANEL_USER=admin
PANEL_PASSWORD=123456
PANEL_PORT=9849
```

`data/panel.conf` 保存页面内可修改的运行配置：

```text
USER=admin
PASSWORD=123456
PROTECTED=docker-panel
```

## 安全说明

该面板挂载 `/var/run/docker.sock`，拥有管理 Docker 的高权限。公开部署时请务必：

- 修改默认密码
- 只对可信 IP 开放面板端口
- 避免把 `.env`、`data/`、服务器密码、SSH 密钥提交到仓库
