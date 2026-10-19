"""Relay persistence contains public routing data, never bearer/key blobs."""
import sqlite3
from ostrelay import config, db


def test_sessions_have_no_secret_columns(fresh_db):
    columns={r['name'] for r in fresh_db.connect().execute('PRAGMA table_info(sessions)').fetchall()}
    assert columns=={'session_id','fp_hash','client_id','created_at','expires_at'}


def test_old_bearer_sessions_are_discarded_during_migration(fresh_db,tmp_path,monkeypatch):
    path=tmp_path/'legacy.sqlite'
    conn=sqlite3.connect(path)
    conn.executescript(db.SCHEMA.replace('client_id           TEXT NOT NULL,','wire_token_enc      BLOB NOT NULL,'))
    conn.execute('INSERT INTO sessions VALUES(?,?,?,?,?)',('old','hash',b'old-encrypted-bearer',0,9999999999))
    conn.commit();conn.close()
    fresh_db.connect().close()
    monkeypatch.setattr(db,'_conn',None)
    monkeypatch.setattr(config.settings(),'db_path',str(path))
    conn=db.connect()
    assert not conn.execute('SELECT * FROM sessions').fetchall()
    assert 'wire_token_enc' not in {r['name'] for r in conn.execute('PRAGMA table_info(sessions)').fetchall()}
