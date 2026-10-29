cd ~/coturn            # каталог, куда был распакован dzcoturn.8 (или dzcoturn.7)
wget https://github.com/caiiiycuk/WebRTC-NET/releases/download/v1.1.2/dzcoturn.9.zip
unzip -o dzcoturn.9.zip  # распаковать поверх старых файлов

Проверьте PUBLISH_URL в .env — он должен быть ровно https://cloud.dos.zone/gulp:

grep PUBLISH_URL .env
sed -i 's|^PUBLISH_URL=.*|PUBLISH_URL=https://cloud.dos.zone/gulp|' .env

docker compose build --pull
docker compose up -d

Если сервер работал на dzcoturn.7 или старше (capacity ни разу не публиковался), один раз включите публикацию capacity:

docker compose stop coturn
docker compose run --rm --no-deps --entrypoint python3 coturn \
  /usr/local/bin/publisher.py --initialize-new-identity
docker compose up -d

Если сервер работал на dzcoturn.8 — ничего инициализировать не нужно, даже если PUBLISH_URL в .env поменялся с cloud.js-dos.com на cloud.dos.zone: с версии 9 история capacity привязана к домену, а не к адресу публикации.

Проверка:
docker compose logs --since 1m coturn | grep -E 'ice-publisher|publisher exited'
# Норма: примерно 12 строк ice-publisher: HTTP 200 в минуту, по одной каждые 5 секунд, и ни одной строки publisher exited.
curl -s http://127.0.0.1:9641/metrics | grep '^turn_quota_used'
docker compose ps   # coturn-server должен быть running (healthy)

Что изменилось с dzcoturn.8:
- Publisher теперь перезапускается entrypoint-ом сам: после любого завершения в логе появляется publisher exited with status N; restarting in 5 seconds и через 5 секунд процесс стартует снова. turnserver при этом не перезапускается, соединения игроков не рвутся. Временные циклы перезапуска, запущенные через docker exec, после пересоздания контейнера исчезают — они больше не нужны.
- Исправлена гонка, из-за которой publisher завершался с startup failed при перегрузке хоста (дедлайн 5 секунд срабатывал во время обработки ошибки цикла). Сообщения cycle failed и startup failed теперь содержат класс и текст исключения.
- Исправлено завершение контейнера: раньше docker stop приводил к падению entrypoint (bash SIGSEGV из-за рекурсивного trap), теперь контейнер останавливается штатно с кодом 0.
- Смена PUBLISH_URL больше не отключает capacity: сохранённый в capacity_state.json адрес стал историческими метаданными, отправка всегда идёт на текущий PUBLISH_URL. Не правьте и не удаляйте state-файл и volume coturn-data.
- STUN-запись в iceServers публикуется только если turnserver.conf её реально обслуживает (realm совпадает с DOMAIN, порт 3478, нет no-stun/secure-stun/no-udp). Стандартный шаблон под это подходит, ничего менять не нужно.
- В .env.example по умолчанию PUBLISH_URL=https://cloud.dos.zone/gulp.

Что важно знать:
- .env сохранится, потому что его нет в архиве. Остальные файлы перезапишутся, включая turnserver.conf.template, docker-compose.yml и nginx-шаблоны. Если вы их правили, сохраните копии заранее.
- PUBLISH_INTERVAL игнорируется, интервал всегда 5 секунд. ADMISSION_LIMIT необязателен, по умолчанию 90% от TOTAL_QUOTA.
- PUBLISH_URL должен начинаться с https://, иначе publisher не стартует и будет перезапускаться каждые 5 секунд с ошибкой publisher requires HTTPS в логе.
- Откат на dzcoturn.8 после смены PUBLISH_URL снова отключит capacity (старая версия требует совпадения URL). Не сбрасывайте state ради отката.
- Порт метрик 9641 доступен только на 127.0.0.1.
- Пересоздание контейнера прерывает текущие TURN-соединения — выбирайте окно с минимальной нагрузкой.
