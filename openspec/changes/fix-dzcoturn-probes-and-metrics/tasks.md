# Tasks

## 1. TLS receiver тестера в форке coturn

- [ ] 1.1 Добавить C regression target в `3rdparty/coturn/tests/CMakeLists.txt`, воспроизводящий настоящий TLS receive path на управляемых chunks: coalesced frames, partial header/body, полный frame с хвостом, STUN/ChannelData/Data indication с padding. Проверить, что исходная версия теряет сообщения, а ожидаемые payload и counters зафиксированы без pacing.
- [ ] 1.2 Исправить TLS control framing в `src/apps/uclient/uclient.c` и при необходимости `session.h`: ограниченное per-connection состояние, сохранение хвоста, обработка готовых frames без нового события, корректные sync/nonblocking ожидания и cleanup. Проверить C-tests и ASan/UBSan на invalid/oversize length, временной неготовности, partial EOF, повторных и параллельных соединениях; raw TCP relay payload не разбирать как STUN.
- [ ] 1.3 Проверить bounded UDP/plain TCP/TLS relay smoke исправленным тестером при штатном интервале, сохранить регрессии DTLS и TCP relay data connection; критерий проверки: ненулевые равные data counters, zero loss, намеренно недоставленный payload не считается успехом.

## 2. Внешний Prometheus

- [ ] 2.1 Изменить default в `coturn-docker/turnserver.conf.template` на `prometheus-address=0.0.0.0` без нового env API, port mapping или изменения publisher. Добавить проверку сгенерированного конфига: порт `9641`, peer deny list и локальный scrape publisher сохранены; существующие Python publisher tests проходят.
- [ ] 2.2 Добавить изолированную Linux namespace/container проверку реального HTTP scrape по non-loopback IPv4 и через `127.0.0.1`, а затем loopback-only opt-out с отказом внешнего подключения. Согласовать все разделы README про метрики, добавить предупреждение HTTP без авторизации, пример host/provider firewall allowlist и явный opt-out; проверить существующие traffic/quota/snapshot families и отсутствие автоматических изменений host firewall. Места, которые сейчас утверждают loopback-only: раздел metrics, одна фраза в `Capacity publication and recovery`, пункт в security, пример `curl` в troubleshooting.

## 3. Проверки и сохранность состояния в инструкциях

- [ ] 3.1 Переписать README `Testing TURN` на внешние UDP/TLS paired-client payload проверки совместимым pinned тестером с bounded timeout, обычным TLS интервалом и строгими counters. Проверить команды на изолированном сервере/клиенте; STUN binding, allocation и TLS handshake отдельно не считаются успехом. Примеры не выводят shared secret/credentials, предупреждают о process argv; исправить путаницу портов `443`/`5349`/`8000`. Добавить диагностику двух разных проблем старых тестеров (`Bad channel number` и ошибочный подсчёт объединённых TLS frames) и описание временного pacing только для старого образа.
- [ ] 3.2 Исправить README backup/restore: фактический Compose project, остановленный writer, private `.env`, сертификаты и весь `coturn-data`, восстановление до запуска publisher. Проверить инструкции на fixture volume с identity, положительным watermark и traffic state: после backup/restore значения и права сохранены, отсутствующая история прежнего сервера не переинициализируется.
- [ ] 3.3 Заменить destructive staging → production инструкции в README и справочном выводе `init.sh` сценарием с сохранением `.env`/volumes, явным production ACME server/lineage и учётом занятого standalone port. Добавить fake Docker/certbot тест документированных команд и подсказки: production server выбран, domain/secret/state неизменны, `down -v` и удаление `.env` не вызываются; `bash -n init.sh` проходит, реальные ACME запросы не выполняются.

## 4. Общая проверка исходников и обёртки

- [ ] 4.1 Прогнать CTest форка с `BUILD_TESTING=ON`, все `coturn-docker/test_*.py`, shell syntax и Compose validation; собрать все Linux-цели HumbleNet и выполнить `ninja -j32 webrtcnet` из `emscripten`. Зафиксировать команды и результаты, не считать пропущенные transport/namespace проверки выполненными; production `.env` не использовать и не выводить полный Compose config с секретами.
- [ ] 4.2 Выполнить `openspec validate fix-dzcoturn-probes-and-metrics --strict`, `git diff --check` в HumbleNet и форке; проверить, что change `harden-capacity-history-against-deadline`, publisher semantics, cloud и VPS не изменены, а основной spec и новые требования не противоречат друг другу.

## 5. Согласованная поставка исправленного тестера

- [ ] 5.1 После отдельного разрешения публикации выпустить новый свободный `release/<coturn-ver>-<N>` через существующий workflow форка и проверить image provenance. Обновить `coturn-docker/Dockerfile` и gitlink `3rdparty/coturn` до одного release commit, README/release notes до фактической версии; проверить соответствие tag/commit/image и не перезаписывать прежние tags/assets. Без разрешения или образа оставить задачу открытой.
- [ ] 5.2 Проверить исправленный `turnutils_uclient` внутри опубликованного pinned image: UDP/TLS smoke без pacing и реальные внешние/локальные метрики default template. Проверить package contents и новый номер, если выпуск архива отдельно разрешён; записать tag, digest и результаты без секретов. Работающие VPS и GitHub wiki не обновлять, непроведённые release проверки не отмечать выполненными.
