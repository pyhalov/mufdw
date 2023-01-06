# Minimal Usable Foreign Data Wrapper

Minimal usable foreign data wrapper is a simple PostgreSQL FDW which can be used in demonstration purposes.
It's main goal is to serve as example data wrapper for all interested students and developers.

## Installation
Ensure that pg_config is installed and usable in your path.

```
make
sudo -i --preserve-env=PATH make install
make install-check
```

## Usage

Create mufdw extension, define server and user mapping.
```
CREATE EXTENSION mufdw;
CREATE SERVER loopback FOREIGN DATA WRAPPER mufdw;
CREATE USER MAPPING FOR PUBLIC SERVER loopback;
```
Create some local table and corresponding foreign table.
```
CREATE TABLE players(id int primary key, nick text, score int);
CREATE FOREIGN TABLE f_players (id int, nick text, score int) server loopback options (table_name 'players', schema_name 'public');
```
Now you can use foreign table to query underlying table.
