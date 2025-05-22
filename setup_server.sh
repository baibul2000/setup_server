#!/bin/bash

# Проверка прав администратора
if [ "$(id -u)" -ne 0 ]; then
  echo "ОШИБКА: Скрипт должен запускаться от root!" >&2
  exit 1
fi

# Создаем все необходимые директории
echo "Создаем системные директории..."
mkdir -p /etc/wireguard    # Для конфигов WireGuard
mkdir -p /opt/vpnbrr       # Для файлов приложения
mkdir -p /var/log/vpnbrr   # Для логов

# Устанавливаем права
chmod 700 /etc/wireguard
chmod 755 /opt/vpnbrr
chmod 755 /var/log/vpnbrr

# Установка зависимостей
echo "Устанавливаем пакеты..."
apt update && apt install -y \
  git \
  nodejs \
  npm \
  wireguard \
  mongodb-org

# Рабочая директория приложения
cd /opt/vpnbrr || {
  echo "ОШИБКА: Не удалось перейти в /opt/vpnbrr" >&2
  exit 1
}

# Клонируем репозиторий
echo "Клонируем репозиторий..."
if ! git clone https://baibul2000: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@github.com/yourname/repo.git/vpnbrr-server .; then 
  echo "ОШИБКА: Не удалось клонировать репозиторий!" >&2
  exit 1
fi

# Настройка WireGuard
echo "Настраиваем WireGuard..."
{
  wg genkey | tee /etc/wireguard/privatekey
  wg pubkey < /etc/wireguard/privatekey > /etc/wireguard/publickey
} || {
  echo "ОШИБКА: Не удалось сгенерировать ключи!" >&2
  exit 1
}

# Создаем конфиг сервера
cat > /etc/wireguard/wg0.conf <<EOF
[Interface]
Address = 10.0.0.1/24
ListenPort = 51820
PrivateKey = $(cat /etc/wireguard/privatekey)
PostUp = iptables -A FORWARD -i wg0 -j ACCEPT; iptables -t nat -A POSTROUTING -o eth0 -j MASQUERADE
PostDown = iptables -D FORWARD -i wg0 -j ACCEPT; iptables -t nat -D POSTROUTING -o eth0 -j MASQUERADE
EOF

# Запускаем сервисы
systemctl enable --now wg-quick@wg0
systemctl enable --now mongod

echo "Установка успешно завершена!"
