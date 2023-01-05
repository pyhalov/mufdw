CREATE EXTENSION mufdw;
CREATE SERVER loopback FOREIGN DATA WRAPPER mufdw;
CREATE USER MAPPING FOR PUBLIC SERVER loopback ;
CREATE TABLE players(id int primary key, nick text, score int);
INSERT INTO players SELECT i, 'nick_'||i, i*10 from generate_series(1,10) i;
-- Error cases
CREATE FOREIGN TABLE f_players (id int, nick text, score int) server loopback;
CREATE FOREIGN TABLE f_players (id int, nick text, score int) server loopback options (table_name 'players');
CREATE FOREIGN TABLE f_players (id int, nick text, score int) server loopback options (schema_name 'public');
CREATE FOREIGN TABLE f_players (id int, nick text, score int) server loopback options (table_name 'players', schema_name 'public', blah 'foo');
-- Simple usage
CREATE FOREIGN TABLE f_players (id int, nick text, score int) server loopback options (table_name 'players', schema_name 'public');
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM f_players WHERE id > 5 ORDER BY id;
SELECT * FROM f_players WHERE id > 5 ORDER BY id;
