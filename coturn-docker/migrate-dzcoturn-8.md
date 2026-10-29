cd ~/coturn            # каталог, куда был распакован dzcoturn.7
wget https://github.com/caiiiycuk/WebRTC-NET/releases/download/v1.1.2/dzcoturn.8.zip
unzip -o dzcoturn.8.zip  # распаковать поверх старых файлов

docker compose build --pull
docker compose up -d

Затем один раз включите публикацию capacity. Это нужно только при переходе с dzcoturn.7 и более старых версий:

docker compose stop coturn

Убедитесь что PUBLISH_URL=https://cloud.dos.zone/gulp в .env

docker compose run --rm --no-deps --entrypoint python3 coturn \
  /usr/local/bin/publisher.py --initialize-new-identity
docker compose up -d

Проверка:
docker compose logs --since 1m coturn | grep ice-publisher
# Если всё в порядке, будет примерно 12 строк ice-publisher: HTTP 200 в минуту, по одной каждые 5 секунд.
curl -s http://127.0.0.1:9641/metrics | grep '^turn_quota_used'

Что важно знать:
- .env сохранится, потому что его нет в архиве. Остальные файлы перезапишутся, включая turnserver.conf.template, docker-compose.yml и nginx-шаблоны. Если вы их правили, сохраните копии заранее.
- Править .env не нужно:
- PUBLISH_INTERVAL теперь игнорируется, интервал всегда 5 секунд.
- ADMISSION_LIMIT необязателен. Если его не задать, берётся 90% от TOTAL_QUOTA.
- PUBLISH_URL должен начинаться с https://. Новый publisher.py отклоняет другие адреса (строка 289). Значение по умолчанию https://cloud.js-dos.com/gulp подходит.
- Команда --initialize-new-identity откажется запускаться второй раз, и это нормально. Не удаляйте volume coturn-data: в нём хранится состояние capacity. Если его потерять, capacity перестанет публироваться, а ICE-серверы и трафик будут публиковаться как раньше.
- Порт метрик 9641 теперь доступен только на 127.0.0.1. Если вы раньше собирали метрики с другой машины, это перестанет работать.
dzcoturn.8.zip на cdn.dos.zone вы должны залить сами. Пока файла там нет, wget не сработает. Обновление живого сервера с dzcoturn.7 я не проверял, проверял только чистую установку из zip.