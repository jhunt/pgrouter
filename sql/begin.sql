DELETE FROM deities WHERE xref = 100;
BEGIN;
INSERT INTO deities(xref, name) VALUES (100, 'Gaia');
SELECT * FROM deities WHERE roman IS NULL;
UPDATE deities SET roman = 'Terra' WHERE xref = 100;
SELECT * FROM deities WHERE roman IS NULL;
COMMIT;
