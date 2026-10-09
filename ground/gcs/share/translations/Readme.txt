How To add translations to GCS
========================================

- Coordinate in the issue tracker to avoid duplicate work.
	https://github.com/reubenfirmin/droniNG/issues

- Add your language to the LANGUAGES line in translations.pro.
  Don't qualify it with a country unless it is reasonable to expect
  country-specific variants.
  Skip this step if updating an existing translation, obviously.

- Run "make gcs_ts". This will complain if you are not building against
  Qt 5.15

- Fire up linguist and do the translation.

- Check in the modified .pro file and _only_ your .ts file.

- Open a pull request in the droniNG repository.

- .qm files are generated as part of the regular build.
