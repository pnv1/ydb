USE plato;

INSERT INTO EmptyOutput WITH MONOTONIC_KEYS
SELECT
    *
FROM
    Input
ORDER BY
    key,
    subkey
;
