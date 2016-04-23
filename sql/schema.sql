DROP TABLE deities;
CREATE TABLE deities (
	xref    INTEGER NOT NULL,
	name    TEXT,
	roman   TEXT,
	sex     CHAR,

	aspect1 TEXT,
	aspect2 TEXT,
	aspect3 TEXT,

	spouse  INTEGER,
	father  INTEGER,
	mother  INTEGER,

	notes   TEXT
);

INSERT INTO deities (xref, name, roman, sex, aspect1, aspect2, aspect3) VALUES
	(1,   'Aphrodite',   'Venus',    'F',  'beauty',      'love',          'desire'),
	(2,   'Apollo',      'Apollo',   'M',  'music',       'arts',          'knowledge'),
	(3,   'Ares',        'Mars',     'M',  'war',         'bloodshed',     'violence'),
	(4,   'Artemis',     'Diana',    'F',  'wilderness',  'animals',       'girls'),
	(5,   'Athena',      'Minerva',  'F',  'war',         'peace',         'wisdon'),
	(6,   'Demeter',     'Ceres',    'F',  'harvest',     'growth',        'nourishment'),
	(7,   'Dionysus',    'Bacchus',  'M',  'wine',        'madness',       'chaos'),
	(8,   'Hades',       'Pluto',    'M',  'death',       'wealth',        NULL),
	(9,   'Hephaestus',  'Vulcan',   'M',  'fire',        'metalworking',  'craft'),
	(10,  'Hera',        'Juno',     'F',  'marriage',    'women',         'childbirth'),
	(11,  'Hermes',      'Mercury',  'M',  'trade',       'language',      'writing'),
	(12,  'Hestia',      'Vesta',    'F',  'hearth',      'home',          'chastity'),
	(13,  'Poseidon',    'Neptune',  'M',  'sea',         'rivers',        'floods'),
	(14,  'Zeus',        'Jupiter',  'M',  'sky',         'lightning',     'law'),
	(15,  'Leto',        'Latona',   'F',  NULL,          NULL,            NULL),
	(16,  'Maia',        'Maia',     'F',  NULL,          NULL,            NULL);

UPDATE deities SET father = (SELECT xref FROM deities WHERE name = 'Zeus')
	WHERE name IN (
		'Aphrodite',
		'Apollo',
		'Ares',
		'Artemis',
		'Athena',
		'Hephaestus',
		'Hermes'
	);
UPDATE deities SET mother = (SELECT xref FROM deities WHERE name = 'Hera')
	WHERE name IN (
		'Ares',
		'Hephaestus'
	);
UPDATE deities SET mother = (SELECT xref FROM deities WHERE name = 'Leto')
	WHERE name IN (
		'Artemis',
		'Apollo'
	);
UPDATE deities SET mother = (SELECT xref FROM deities WHERE name = 'Maia')
	WHERE name IN (
		'Hermes'P
	);

SELECT * FROM deities;
