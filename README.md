# Минимальная утилита контейнеризации на C

Учебный проект: изолированный запуск процессов через пространства имён Linux
(PID, mount, user, UTS, IPC, network) с rootless-поддержкой и pivot_root.

## Что это

Этот проект представляет собой минимальный контейнерный рантайм на языке C.
Он использует Linux namespaces и базовые системные вызовы для запуска процесса
в изолированном окружении, где он видит собственное PID-пространство,
отдельное mount-пространство, собственный hostname, отдельный сеть-стек и т.д.

Главная цель — показать, как устроен "ядро контейнеризации" без сложных
инструментов вроде cgroups, overlayfs, реестров образов или полноценного
сетевого стека Docker.

## Возможности

- Создание всех шести пространств имён одним `unshare(2)`:
  `CLONE_NEWUSER | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNS | CLONE_NEWNET | CLONE_NEWPID`
- Rootless-запуск: отображение UID/GID через `/proc/<pid>/uid_map` и `gid_map`
- Синхронизация родителя и потомка через два pipe:
  дочерний процесс ждёт, пока родитель запишет маппинги UID/GID
- Второй `fork()` для получения PID 1 в новом PID-namespace
- `pivot_root(2)` (с fallback на `chroot(2)`) для смены корня
- Монтирование `/proc` и `/dev` (tmpfs + базовые device-ноды)
- Замена образа процесса через `execv(3)`

## Структура проекта

- `runtime.c` — основной рантайм и логика изоляции
- `Makefile` — сборка исполняемого файла
- `Dockerfile` — сборка контейнерной среды для запуска проекта

## Сборка

### Локально

```bash
make
./runtime run /tmp/alpine /bin/ls
```

### В Docker

```bash
docker build -t container-runtime .
docker run --rm --privileged container-runtime run /opt/alpine-rootfs /bin/ls
```

Флаг `--privileged` нужен, чтобы внешний Docker-контейнер позволил создавать
вложенные пространства имён.

## Подготовка rootfs

```bash
mkdir -p /tmp/alpine
docker export $(docker create alpine:latest) | tar -xf - -C /tmp/alpine
```

## Использование

```bash
./runtime run <rootfs> <command> [args...]
```

Пример:

```bash
./runtime run /tmp/alpine /bin/sh
./runtime run /tmp/alpine /bin/ls -la
```

## Проверка изоляции

### Список файлов rootfs (mount namespace)

```bash
sudo docker run --rm --privileged container-runtime run /opt/alpine-rootfs /bin/ls
```

Ожидание: `bin dev etc home lib ...` — содержимое rootfs, не хоста.

### PID-изоляция

```bash
sudo docker run --rm --privileged container-runtime run /opt/alpine-rootfs /bin/ps
```

Ожидание:

```text
PID   USER     TIME  COMMAND
    1 root      0:00 /bin/ps
```

Процесс видит себя под PID 1.

### Сравнение с хостом

```bash
# Хост
id          # uid=1000(pov) gid=1000(pov)
hostname    # ubuntu2404

# Контейнер
sudo docker run --rm --privileged container-runtime run /opt/alpine-rootfs /bin/sh -c "id && hostname"
# uid=0(root) gid=0(root)
# container
```

### Все пространства имён сразу

```bash
sudo docker run --rm --privileged container-runtime run /opt/alpine-rootfs /bin/sh -c "
  echo '--- id ---';        id;
  echo '--- hostname ---';  hostname;
  echo '--- ps ---';        ps;
  echo '--- ls / ---';      ls /;
  echo '--- ip link ---';   ip link 2>/dev/null || echo 'no ip'
"
```

Фактический вывод:

```text
--- id ---
uid=0(root) gid=0(root) groups=0(root)
--- hostname ---
container
--- ps ---
PID   USER     TIME  COMMAND
    1 root      0:00 /bin/sh -c ...
    4 root      0:00 ps
--- ls / ---
bin dev etc home lib media mnt opt proc root run sbin srv sys tmp usr var
--- ip link ---
1: lo: <LOOPBACK> mtu 65536 qdisc noop state DOWN qlen 1000
```

| Пространство имён | Проверка | Результат |
|---|---|---|
| User | `id` | `uid=0(root)`, хотя хост — `uid=1000(pov)` |
| UTS | `hostname` | `container`, хотя хост — `ubuntu2404` |
| PID | `ps` | только процессы контейнера, первый — PID 1 |
| Mount | `ls /` | файлы rootfs, а не хоста |
| Network | `ip link` | только loopback `lo`, без интерфейсов хоста |
| IPC | — | изолирован по построению (не проверяется утилитами busybox) |

## Замечание о работе внутри Docker

При запуске рантайма внутри `--privileged`-контейнера user namespace
(`CLONE_NEWUSER`) не создаётся, так как процесс уже является root.
Вложенный userns второго уровня приводит к тому, что ядро запрещает
монтирование `procfs` и создание device-нод (`EPERM`) — это известное
ограничение ядра Linux.

Утилита определяет необходимость userns по `getuid()`:
- на хосте от обычного пользователя — создаёт userns и пишет маппинги;
- внутри Docker от root — пропускает.

Это позволяет одной и той же программе работать в обоих сценариях.
