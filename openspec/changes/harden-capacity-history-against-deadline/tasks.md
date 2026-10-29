# Tasks

## 1. Устойчивость истории capacity в `publisher.py`

- [ ] 1.1 Вынести разбор и проверку содержимого `capacity_state.json` (версия, форма `binding`, домен, тип и диапазон `attemptedThroughMs`) из `Publisher.__init__` в одну функцию и использовать её в `__init__` и в `capacity()`; проверить, что `python3 -m unittest -v test_publisher.py` проходит без изменений в существующих тестах.
- [ ] 1.2 В `capacity()` добавить `except TimeoutError: raise` перед обоими `except (OSError, ValueError)`; в `test_publisher.py` добавить тест: чтение истории поднимает `TimeoutError` → `capacity()` пробрасывает его, следующий вызов с более новым снимком возвращает `known`; `durable_write` подменён обёрткой «настоящая запись, затем `TimeoutError`» → следующий `capacity()` с тем же снимком даёт `unknown` с барьером из файла, с более новым — `known`.
- [ ] 1.3 Заменить сравнение файла «целиком» на правила «только вперёд»: другой `binding`, невалидное содержимое или барьер меньше запомненного → `recovered = False`; барьер больше → принять в память; равен → без изменений. Тест: три варианта перезаписи файла (больше, меньше, другой `binding`) с ожидаемыми `known`/`unknown`/`None` и неизменным файлом при отключении; существующие `test_external_history_change_disables_known_regardless_of_url` и `test_durable_barrier_before_send_restart_and_lost_ack` проходят.
- [ ] 1.4 Ошибку чтения файла (`OSError`, кроме `TimeoutError`) превратить в блок на один цикл без изменения `recovered`/`attempted`; тест: `read_text` один раз поднимает `OSError` → `unknown` с прежним барьером, следующий цикл с более новым снимком → `known`.
- [ ] 1.5 При переходе `recovered` в `False` в рантайме печатать одну строку `ice-publisher: capacity history unavailable: <причина>` в stderr без содержимого файла и окружения; тест с перехватом stderr: ровно одна строка при отключении и ни одной при последующих вызовах; при принятии барьера вперёд строк нет.
- [ ] 1.6 Обновить раздел истории capacity в `coturn-docker/README.md` (английский): дедлайн не отключает историю, барьер из файла принимается только вперёд, временная ошибка чтения блокирует один цикл, формат сообщения об отключении; проверить описание против тестов.

## 2. Рецепт conntrack в README

- [ ] 2.1 В разделе подготовки хоста `coturn-docker/README.md` добавить `echo nf_conntrack > /etc/modules-load.d/nf_conntrack.conf` перед `sysctl --system`, проверку `sysctl net.netfilter.nf_conntrack_max net.netfilter.nf_conntrack_count` после него и предупреждение, что без загруженного модуля ключ `nf_conntrack_max` молча пропускается; для VM с памятью меньше 1 ГБ указать ориентир 65536 (≈256 байт на запись). Проверить, что команды в README выполняются как написаны (`bash -n` на извлечённом блоке).

## 3. Интеграционная проверка

- [ ] 3.1 Из `coturn-docker` выполнить `python3 -m unittest -v test_publisher.py test_entrypoint.py`; подтвердить, что проверки дедлайна, блокировки, внешнего изменения истории и смены URL проходят без изменения формата state-файла.
- [ ] 3.2 Выполнить обязательные проверки сборки из `AGENTS.md`: сборку всех целей проекта и `ninja -j32 webrtcnet` из `emscripten`; зафиксировать результат и точные блокеры окружения, если запуск невозможен.
- [ ] 3.3 Проверить итоговый diff `git diff` и `git status --short`: только `publisher.py` (метод `capacity()` и вынесенная проверка файла), `test_publisher.py`, README и артефакты изменения; без новых зависимостей, без правок `entrypoint.sh`, формата файла и cloud, без деплоя на серверы.
