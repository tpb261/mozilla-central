/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

/**
 * This file tests migration invariants from a database with schema version 11
 * that was then downgraded to a database with a schema version 10.  Places
 * should then migrate this database to one with a schema version of 11.
 */

////////////////////////////////////////////////////////////////////////////////
//// Test Functions

function test_initial_state()
{
  // Mostly sanity checks our starting DB to make sure it's setup as we expect
  // it to be.
  let dbFile = gProfD.clone();
  dbFile.append(kDBName);
  let db = Services.storage.openUnsharedDatabase(dbFile);

  let stmt = db.createStatement("PRAGMA journal_mode");
  do_check_true(stmt.executeStep());
  // WAL journal mode should have been unset this database when it was migrated
  // down to v10.
  do_check_neq(stmt.getString(0).toLowerCase(), "wal");
  stmt.finalize();

  do_check_true(db.indexExists("moz_bookmarks_guid_uniqueindex"));
  do_check_true(db.indexExists("moz_places_guid_uniqueindex"));

  // There should be a non-zero amount of bookmarks without a guid.
  stmt = db.createStatement(
    "SELECT COUNT(1) "
  + "FROM moz_bookmarks "
  + "WHERE guid IS NULL "
  );
  do_check_true(stmt.executeStep());
  do_check_neq(stmt.getInt32(0), 0);
  stmt.finalize();

  // There should be a non-zero amount of places without a guid.
  stmt = db.createStatement(
    "SELECT COUNT(1) "
  + "FROM moz_places "
  + "WHERE guid IS NULL "
  );
  do_check_true(stmt.executeStep());
  do_check_neq(stmt.getInt32(0), 0);
  stmt.finalize();

  // Check our schema version to make sure it is actually at 10.
  do_check_eq(db.schemaVersion, 10);

  db.close();
  run_next_test();
}

function test_bookmark_guids_non_null()
{
  // First, sanity check that we have a non-zero amount of bookmarks.  If
  // migration failed, we would have zero.
  let stmt = DBConn().createStatement(
    "SELECT COUNT(1) "
  + "FROM moz_bookmarks "
  );
  do_check_true(stmt.executeStep());
  do_check_neq(stmt.getInt32(0), 0);
  stmt.finalize();

  // Now, make sure we have no NULL guid entries.
  stmt = DBConn().createStatement(
    "SELECT guid "
  + "FROM moz_bookmarks "
  + "WHERE guid IS NULL "
  );
  do_check_false(stmt.executeStep());
  stmt.finalize();
  run_next_test();
}

function test_place_guids_non_null()
{
  // First, sanity check that we have a non-zero amount of places.  If migration
  // failed, we would have zero.
  let stmt = DBConn().createStatement(
    "SELECT COUNT(1) "
  + "FROM moz_places "
  );
  do_check_true(stmt.executeStep());
  do_check_neq(stmt.getInt32(0), 0);
  stmt.finalize();

  // Now, make sure we have no NULL guid entry.
  stmt = DBConn().createStatement(
    "SELECT guid "
  + "FROM moz_places "
  + "WHERE guid IS NULL "
  );
  do_check_false(stmt.executeStep());
  stmt.finalize();
  run_next_test();
}

function test_final_state()
{
  // We open a new database mostly so that we can check that the settings were
  // actually saved.
  let dbFile = gProfD.clone();
  dbFile.append(kDBName);
  let db = Services.storage.openUnsharedDatabase(dbFile);

  do_check_eq(db.schemaVersion, 11);

  db.close();
  run_next_test();
}

////////////////////////////////////////////////////////////////////////////////
//// Test Runner

let tests = [
  test_initial_state,
  test_bookmark_guids_non_null,
  test_place_guids_non_null,
  test_final_state,
];
let index = 0;

function run_next_test()
{
  function _run_next_test() {
    if (index < tests.length) {
      do_test_pending();
      print("TEST-INFO | " + _TEST_FILE + " | Starting " + tests[index].name);

      // Exceptions do not kill asynchronous tests, so they'll time out.
      try {
        tests[index++]();
      }
      catch (e) {
        do_throw(e);
      }
    }

    do_test_finished();
  }

  // For sane stacks during failures, we execute this code soon, but not now.
  do_execute_soon(_run_next_test);
}

function run_test()
{
  setPlacesDatabase("places_v10_from_v11.sqlite");
  do_test_pending();
  run_next_test();
}
