# clunkersystem v0.0.1

Clunkersystem is an in-memory filesystem.  It can be easily wiped and staged to fail after a specified number of operations.  Expected uses are doing in-memory filesystem things as well as testing filesystem-transactional processes.

Based on the testing technique described [here](http://www.sqlite.org/testing.html).

Please don't store important files in your clunkersystem mounts.

## Usage

#### Starting
```bash
CLUNKER_PORT=4522 clunker MOUNTPOINT
```
This will start a clunker mount at `MOUNTPOINT`.  The environment variable `CLUNKER_PORT` determines which TCP port is used to control out-of-band filesystem operations.

Send `SIGINT`, `SIGTERM`, or `SIGHUP` to gracefully unmount and terminate.

#### TCP Control

Out of band filesystem operations are done using a [luxem](https://github.com/Rendaw/luxem) API.

##### Mass erase
```luxem
(clean),
```

Will respond in the format:
```luxem
(clean_result) true,
```

The `true` indicates success.

##### Set failure countdown
```luxem
(set_count) 2000,
```

Will respond in the format:
```luxem
(set_result) true,
```

The `true` indicates success.  A count of `-1` disables the failure countdown - all operations will succeed.  Every operation performed on the filesystem (both reading and writing, statting files, etc) will decrement the count.

##### Get failure countdown
```luxem
(get_count),
```

Returns the count in the format:
```luxem
(count) 137,
```

## Installation

### Arch Linux

```bash
cd packaging/arch64
./create.sh
pacman -U clunkersystem*.pkg.tar.xz
```

### Other Linux

```bash
git submodule update --init
cp tup.template.config tup.config
tup init
tup
```

The compiled binary will be `app/clunker` and has no other in-project dependencies, so it can be moved elsewhere on the system freely.

