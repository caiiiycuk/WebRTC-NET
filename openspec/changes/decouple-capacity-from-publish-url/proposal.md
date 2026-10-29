# Proposal

## Why

На `anfit.pp.ua` смена `PUBLISH_URL` отключила capacity при исправных метриках: publisher сравнивает адрес назначения с `binding.publishUrl` в сохранённой истории. Смена адреса публикации того же TURN-домена не должна требовать ручного редактирования состояния или повторной инициализации.

## What Changes

- Не отключать capacity из-за отличия текущего `PUBLISH_URL` от сохранённого `binding.publishUrl`.
- Автоматически принимать существующий корректный файл состояния того же `DOMAIN`, сохраняя `attemptedThroughMs` и нынешние правила `unknown`/`known` после перезапуска.
- Сохранить формат файла, проверки домена, целостности истории, внешних изменений файла, HTTPS и метрик.
- Добавить регрессионные тесты и уточнить README: URL является адресом доставки, а не идентификатором истории capacity.

## Capabilities

### New Capabilities

Нет.

### Modified Capabilities

- `coturn-docker-image`: определить продолжение публикации capacity после смены URL без сброса истории и ослабления остальных проверок.

## Impact

- Код: `coturn-docker/publisher.py`; тесты: `coturn-docker/test_publisher.py`; документация: `coturn-docker/README.md`.
- API Cloud, payload capacity, зависимости, Docker Compose и TURN-конфигурация не меняются.
- Развёртывание, перезапуск серверов и ручная миграция production-состояния не входят в change.
- Незакоммиченные изменения и отдельный change `restart-coturn-publisher` сохраняются; управление процессами publisher здесь не меняется.
