# whitehttpd_c - Freestanding Си на nolibc

Версия **1.0.0**. Переносимый сервер статики без libc.
Второй продукт проекта: `../final-4092/` - ассемблерный эталон размера
на GAS MIPS32r2 BE.

**Сравнивать их по размеру и набору функций некорректно: разные задачи.**
`final-4092` доказывает достижимость одной страницы памяти и не развивается.
`whitehttpd_c` развивается и покрывает современные потребности.

## Сборка

```sh
make            # x86-64
make mipsbe     # MIPS32r2 Big-Endian
make mipsel     # MIPS32r2 Little-Endian
```

### Размеры (2026-10-27)

| Бинарник | Байт |
| --- | ---: |
| `whitehttpd-nolibc-x86_64` | 17 008 |
| `whitehttpd-nolibc-mips32be` | 20 908 |
| `whitehttpd-nolibc-mips32el` | 20 908 |

## Инструменты

| Инструмент | Версия | Роль |
| --- | --- | --- |
| gcc (native) | `Debian 12.2.0-14+deb12u1` 12.2.0 | сборка x86-64 |
| mipsel-linux-gnu-gcc | `Debian 12.2.0-14` 12.2.0 | кросс-сборка MIPS32 |
| GNU ld | 2.40 | компоновка |
| nolibc.h | вендорен, `third_party/nolibc` | freestanding-рантайм |
| Python | 3.11.2 | `tools/sstrip.py` |
| qemu-mips-static | 7.2.22 | runtime-тесты MIPS |
| wrk | 4.2.0 | бенчмарки |

## Параметры компиляции

### Общие флаги

```
-Oz -Wall -Wextra -Werror -fno-pic -fno-pie
-ffreestanding -fno-builtin -fno-stack-protector
-fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident
-ffunction-sections -fdata-sections
-isystem third_party/nolibc -Isrc
```

### Флаги компоновщика

```
-nostdlib -nostartfiles -static -no-pie
-Wl,--gc-sections -Wl,-s -Wl,-z,noseparate-code
-Wl,-z,noexecstack -Wl,--build-id=none -Wl,-e,_start
```

Для MIPS дополнительно:

```
-march=mips32r2 -mabi=32 -mno-abicalls -G 0
-isystem /usr/mipsel-linux-gnu/include -Wl,-e,__start
```

### Зачем каждый флаг

| Флаг | Причина |
| --- | --- |
| `-Oz` | размер важнее скорости; потерь пропускной способности не замерено (см. ниже) |
| `-ffreestanding -fno-builtin` | нет libc, нет встроенных подстановок |
| `-fno-pic -fno-pie` | убирает `.data.rel.ro`, уменьшает RW-сегмент |
| `-fno-stack-protector` | нет зависимости от `__stack_chk_fail` |
| `-fno-asynchronous-unwind-tables -fno-unwind-tables` | убирает `.eh_frame` |
| `-ffunction-sections -fdata-sections` + `--gc-sections` | выбрасывает неиспользуемый код |
| `-Wl,-z,noseparate-code` | нет выравнивания по страницам между RX и RW |
| `-Wl,-s` | стрип символов |
| `-G 0` | MIPS: без `$gp`-адресации, иначе SIGBUS без `crt1.o` |

### Стрип

Две стадии:

1. `-Wl,-s` на этапе компоновки удаляет таблицу символов.
2. `tools/sstrip.py` обнуляет `e_shoff`, `e_shentsize`, `e_shnum`,
   `e_shstrndx` и усекает файл по концу последнего сегмента с данными.
   Таблица секций нужна только GDB и objdump, ядро её игнорирует.
   Убирает 424 байта (x86-64) или 460 байт (MIPS).

В результате таблицы секций нет вообще, поэтому `readelf -S` сообщает
"no sections in this file". Для сегментов использовать `readelf -l`.

### LTO: проверен, не применяется

`-flto` замерен и отвергнут:

| Цель | `-Oz` | `-Oz -flto` | Результат |
| --- | ---: | ---: | --- |
| x86-64 | 17 008 | 16 988 | -20 байт, работает |
| MIPS32 BE | 20 908 | сборка падает | `undefined reference to memcpy` |

LTO работает после инлайнинга и заново генерирует вызовы `memcpy`, которые
freestanding-компоновка разрешить не может. Выигрыш на x86-64 - 20 байт
(0.1%), что не оправдывает отдельный путь сборки под каждую архитектуру.

## Возможности

| Возможность | Детали |
| --- | --- |
| Версия | `whitehttpd 1.0.0`, `--version`, `Server: whitehttpd/1.0.0` |
| Методы | `GET`, `HEAD` без тела на всех кодах, `OPTIONS` с `Allow` |
| Отказ методов | `POST`/`PUT`/`DELETE`/`PATCH` -> `405` + `Allow`, прочее -> `501` |
| Протокол | HTTP/1.0 и 1.1, Keep-Alive, pipelining |
| Листинг каталогов | с HTML-экранированием, кап 64 КиБ |
| Range | один диапазон, `206` и `416` |
| Валидаторы | `ETag`, `Last-Modified`, `304`, `If-None-Match`, `If-Modified-Since`, `If-Range` |
| Кэширование | `Cache-Control: max-age=60` на кодах < 400 |
| MIME | 17 расширений |
| Безопасность путей | покомпонентный `openat` + `O_NOFOLLOW` на каждом шаге |
| Dotfiles | `403`, кроме `/.well-known/` (нужен для ACME HTTP-01) |
| Валидация запроса | `Host`, CRLF, obs-fold, `Transfer-Encoding`, `Content-Length` |
| Сеть | dual-stack IPv6 + IPv4, `--addr` |
| Масштабирование | prefork `--workers 1..16`, epoll, `MAXFD 128` |
| Таймауты | запрос 15 с, простой 30 с, keep-alive 5 с |
| Логи | `--log FILE`: access в CLF + ошибки; перезапись при 256 КБ; по умолчанию выключены |
| Заголовки ответа | `Date`, `Accept-Ranges: bytes`, `X-Content-Type-Options: nosniff` |
| Индексный файл | `--index NAME`, по умолчанию `index.html` |
| Управление листингом | `--no-listing` возвращает `403` вместо списка |
| PID-файл | `--pidfile FILE` для init-скриптов |

## CLI

```
whitehttpd [PATH] [PORT] [OPTIONS]

  -h, --help           показать справку
  -v, --version        версия, автор, лицензия
      --port N         порт 1..65535
      --addr ADDR      адрес привязки, IPv4 или IPv6
      --workers N      prefork 1..16
      --index NAME     индексный файл
      --no-listing     отключить листинг каталогов
      --daemon         уйти в фон
      --pidfile FILE   записать PID мастера
      --log FILE       access и error лог
```

Примеры:

```sh
whitehttpd /var/www
whitehttpd /var/www --port 80 --daemon --pidfile /run/w.pid
whitehttpd /srv --addr 127.0.0.1 --workers 1 --log /tmp/w.log
whitehttpd /srv --index home.html --no-listing
```

## Чего нет

TLS (терминируется в stunnel или nginx спереди), CGI, WebSocket, HTTP/2,
gzip, Basic-аутентификация, chroot, конфиг-файл, множественные диапазоны.

## Память

`MAXFD 128`, буфер запроса 2 КиБ, буфер ответа 2 КиБ, два слота страниц по 64 КиБ:

| Параметр | Значение |
| --- | ---: |
| Виртуальный сегмент данных | 656 КиБ на воркер |
| `VmRSS` простой (x86-64) | 4 КиБ |
| `VmRSS` простой (роутер, ядро 3.3.8) | 28 КиБ |
| `VmRSS` при 100 открытых соединениях (роутер) | 488 КиБ |

Замерено на ASUS RT-AC57U V3. Ядро выделяет страницы `.bss` по требованию,
поэтому простой процесс мал; цифры растут только с реально удерживаемыми
соединениями. Уменьшение `MAXFD` с 256 до 128 снизило footprint под нагрузкой
с 624 до 488 КиБ на воркер.

Буферы в `.bss`; ядро выделяет страницы по требованию, поэтому простой
процесс занимает килобайты, а не мегабайты.

## Тесты

```sh
python3 tests/test_tier1.py                        # x86-64
python3 tests/test_tier2.py                        # x86-64
python3 tests/test_tier1.py --binary build/whitehttpd-nolibc-mips32be \
    --runner qemu-mips-static                      # MIPS под QEMU
python3 ../tests/regression.py --mips              # проектная регрессия
```

| Набор | Проверок |
| --- | ---: |
| `test_tier1.py` | 34 |
| `test_tier2.py` | 52 |
| `tests/regression.py` | 178 |

## Известные ограничения

- `MAXFD 128` на воркер: 512 соединений при 4 воркерах.
- Slowloris ограничен таймаутом запроса 15 с, но дескрипторы закрываются,
  а не возвращаются в пул для переиспользования.
- Dotfiles блокируются целиком, кроме `/.well-known/`.
- Логи пишутся в один файл всеми воркерами через `O_APPEND`.
- При превышении 256 КБ файл обнуляется и пишется заново; предыдущая
  копия не сохраняется. Это перезапись, а не ротация.
- Множественные диапазоны не поддерживаются: `bytes=0-1,4-5` получает
  `200` с полным телом. RFC 7233 §3.1 прямо разрешает игнорировать Range.
- `SO_REUSEPORT` не используется: появился в Linux 3.9, а на целевом
  роутере ядро 3.3.8 возвращает `ENOPROTOOPT` (проверено на железе).
  Воркеры делят один слушающий дескриптор. Замеренный дисбаланс на
  x86-64 при 6000 запросах: stime 21/10/15/20 тиков, до двух раз между
  воркерами.

## Лицензия

AGPL-3.0-or-later. Доступна коммерческая OEM-лицензия.
Автор: Ivan Svarkovsky <ivansvarkovsky@gmail.com>

## Справочник по API

Исходники размечены под Doxygen. Сборка: `doxygen Doxyfile` из корня
репозитория, результат в `docs/doxygen/html/index.html`.
