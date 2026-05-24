#!/bin/bash
set -e

ROLE=${FASTDFS_ROLE:-all}
TRACKER_SERVERS=${TRACKER_SERVERS:-172.30.0.3:22122}
GROUP_NAME=${GROUP_NAME:-group1}
STORAGE_IP=${STORAGE_IP:-}

configure_tracker_servers() {
    local conf_file=$1
    local tmp_file

    sed -i '/^tracker_server=/d' "$conf_file"
    tmp_file=$(mktemp)
    IFS=',' read -ra servers <<< "$TRACKER_SERVERS"
    awk -v servers="${servers[*]}" '
        /^# tracker_server/ {
            print
            count = split(servers, tracker_servers, " ")
            for (i = 1; i <= count; i++) {
                print "tracker_server=" tracker_servers[i]
            }
            next
        }
        { print }
    ' "$conf_file" > "$tmp_file"
    cat "$tmp_file" > "$conf_file"
    rm -f "$tmp_file"
}

configure_storage() {
    sed -i "s/^group_name=.*/group_name=${GROUP_NAME}/" /etc/fdfs/storage.conf

    if [ -n "$STORAGE_IP" ]; then
        sed -i "s/^bind_addr=.*/bind_addr=${STORAGE_IP}/" /etc/fdfs/storage.conf
    fi

    configure_tracker_servers /etc/fdfs/storage.conf
    configure_tracker_servers /etc/fdfs/mod_fastdfs.conf
    configure_tracker_servers /etc/fdfs/client.conf
}

if [ "$ROLE" = "tracker" ] || [ "$ROLE" = "all" ]; then
    echo "启动 tracker..."
    /usr/bin/fdfs_trackerd /etc/fdfs/tracker.conf start
    sleep 3
fi

if [ "$ROLE" = "storage" ] || [ "$ROLE" = "all" ]; then
    configure_storage
    echo "启动 storage: group=${GROUP_NAME}, trackers=${TRACKER_SERVERS}"
    /usr/bin/fdfs_storaged /etc/fdfs/storage.conf start
    echo "等待 storage 初始化 (首次启动需要创建子目录)..."
    sleep 15
fi

echo "查看 tracker 是否启动:"
lsof -i:22122 || echo "tracker 未启动"
echo "查看 storage 是否启动:"
lsof -i:23000 || echo "storage 未启动"

if [ "$ROLE" = "tracker" ]; then
    echo "tracker 容器保持运行..."
    tail -F /fastdfs_data_and_log/tracker/logs/trackerd.log
else
    # 启动 nginx (前台运行，保持容器存活)
    echo "启动 nginx..."
    chmod +x /usr/local/nginx/sbin/nginx
    /usr/local/nginx/sbin/nginx -g "daemon off;"
fi
