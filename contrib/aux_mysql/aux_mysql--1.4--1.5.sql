
CREATE OR REPLACE FUNCTION mysql.uuid()
RETURNS text
AS
$$
BEGIN
    return gen_random_uuid();
END;
$$
LANGUAGE plpgsql;

