
CREATE OR REPLACE FUNCTION mysql.uuid()
RETURNS text
AS
$$
BEGIN
    return gen_random_uuid();
END;
$$
LANGUAGE plpgsql;


create or replace function mysql.setval(regclass, bigint, boolean)
returns pg_catalog.int8
as 'MODULE_PATHNAME', 'mysSetval3Oid'
language C
STRICT;

