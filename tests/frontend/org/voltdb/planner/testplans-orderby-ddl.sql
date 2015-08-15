CREATE TABLE T (
	T_D0   INTEGER NOT NULL,
	T_D1   INTEGER NOT NULL,
	T_D2   INTEGER NOT NULL,
	CONSTRAINT T_TREE PRIMARY KEY (T_D0, T_D1)
);

CREATE TABLE T2 (
    T_D0 INTEGER NOT NULL,
    T_D1 INTEGER NOT NULL,
    T_D2 INTEGER NOT NULL,
    CONSTRAINT T2_TREE PRIMARY KEY (T_D0, T_D1)
);

CREATE TABLE P (
    P_D0 INTEGER NOT NULL,
    P_D1 INTEGER NOT NULL,
    P_D2 INTEGER NOT NULL,
    P_D3 INTEGER NOT NULL,
);

CREATE INDEX P_D0_IDX ON P (P_D0);
CREATE INDEX P_D1_IDX ON P (P_D1);
CREATE INDEX P_D32_IDX ON P (P_D3, P_D2);
CREATE INDEX P_D32_10_IDX ON P (P_D3 / 10, P_D2);

PARTITION TABLE P ON COLUMN P_D0;

CREATE TABLE P1 (
    P1_D0 INTEGER NOT NULL,
    P1_D1 INTEGER NOT NULL,
    P1_D2 INTEGER NOT NULL,
);

PARTITION TABLE P1 ON COLUMN P1_D0;

CREATE VIEW V_P1 (V_G1, V_G2, V_CNT, V_sum_age)
    AS SELECT P1_D1, P1_D2, count(*), sum(P1_D1)  FROM P1
    GROUP BY P1_D1, P1_D2;
CREATE VIEW V_P1_ABS (V_G1, V_G2, V_CNT, V_sum_age)
    AS SELECT abs(P1_D1), P1_D2, count(*), sum(P1_D1)  FROM P1
    GROUP BY abs(P1_D1), P1_D2;

CREATE TABLE Tnokey (
	T_D0   INTEGER NOT NULL,
	T_D1   INTEGER NOT NULL,
	T_D2   INTEGER NOT NULL,
);

CREATE TABLE Tnokey2 (
    T_D0 INTEGER NOT NULL,
    T_D1 INTEGER NOT NULL,
    T_D2 INTEGER NOT NULL
);

CREATE TABLE Tmanykeys (
	T_D0   INTEGER NOT NULL,
	T_D1   INTEGER NOT NULL,
	T_D2   INTEGER NOT NULL,
	CONSTRAINT T_TREE_3 PRIMARY KEY (T_D0, T_D1, T_D2)
);
