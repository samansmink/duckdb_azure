# Test data
This directory contains test data that is uploaded to Azure tests servers in CI. What this means is that when adding
files in this directory, the `test/sql/cloud/cloud_integrity_check.test` should be updated, otherwise CI will fail.