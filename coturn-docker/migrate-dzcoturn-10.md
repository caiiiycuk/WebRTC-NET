cd ~/coturn            # каталог, куда был распакован dzcoturn.9
wget https://github.com/caiiiycuk/WebRTC-NET/releases/latest/download/dzcoturn.10.zip
unzip -o dzcoturn.10.zip  # распаковать поверх старых файлов

docker compose build --pull
docker compose up -d --force-recreate coturn

Ничего инициализировать не нужно: .env, тома и история capacity сохраняются. Не выполнять docker compose down -v и не удалять .env.

Проверка:
docker compose logs --since 1m coturn | grep -E 'ice-publisher|publisher exited'
# Норма: примерно 12 строк ice-publisher: HTTP 200 в минуту и ни одной строки publisher exited.
ss -ltnp | grep 9641
# Норма: 0.0.0.0:9641 (turnserver). Порт в файрволе при этом закрыт, пока вы его не откроете.
docker compose ps   # coturn-server должен быть running (healthy)

Что изменилось с dzcoturn.9:
- BREAKING: Prometheus-метрики (HTTP без авторизации и TLS, TCP 9641) теперь слушают 0.0.0.0, а не 127.0.0.1, чтобы их мог читать внешний Prometheus. Файрвол не меняется автоматически — порт остаётся закрыт, пока вы не разрешите его только адресу мониторинга:
    ufw allow from <monitoring-ip> to any port 9641 proto tcp
  Такое же правило по источнику — в облачном файрволе провайдера. Никогда не открывайте 9641 для любого источника.
  Чтобы вернуть прежний локальный режим: prometheus-address=127.0.0.1 в turnserver.conf.template и docker compose up -d --force-recreate coturn. Publisher в обоих режимах читает метрики через 127.0.0.1.
- Образ ghcr.io/caiiiycuk/coturn:4.18.0-4: turnutils_uclient корректно собирает TLS-кадры. Раньше при проверке TURNS по TLS тестер из образа 4.18.0-3 терял сообщения при штатном интервале 20 мс и показывал Total lost packets, хотя сервер был исправен; обход -z 1000 больше не нужен.
- README: раздел Testing TURN описывает внешнюю UDP/TLS проверку relay с отдельной машины тестером из закреплённого образа (успех = равные ненулевые tot_send_msgs/tot_recv_msgs и 0 потерь), без вывода секрета в лог.
- README: backup/restore теперь включает том coturn-data (история capacity и traffic), имена томов берутся из фактического Compose project.
- README и подсказка init.sh: переход со staging на production сертификаты без down -v и без удаления .env — certbot certonly --webroot ... --force-renewal через работающий nginx, затем docker compose restart nginx coturn.
