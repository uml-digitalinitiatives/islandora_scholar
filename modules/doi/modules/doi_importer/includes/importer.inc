<?php

/**
 * @file
 * Defines a class for importing DOI citations.
 */

class DOIImporter extends IslandoraBatchImporter {

  protected $itemClass = 'DOIImportObject';

  /**
   * Get the number of items to import from $this->file.
   *
   * @see IslandoraBatchImporter::getNumber()
   */
  public function getNumber() {
    // "file" is perhaps a misnomer.
    return count($this->file);
  }

  /**
   * Get the form for this importer.
   *
   * @see IslandoraBatchImporter::getForm()
   */
  public static function getForm(array &$form_state) {
    // Get the list of valid content models.
    $parent = islandora_object_load($form_state['storage']['parent_pid']);

    $cm_options = array();
    $ns_options = array(
      NULL => t('Default namespace'),
    );
    if (isset($parent['COLLECTION_POLICY'])) {
      $policy_stream = $parent['COLLECTION_POLICY'];
      $policy = new CollectionPolicy($policy_stream->content);

      $cm_options = array();
      $ns_options = array(NULL => t('Default namespace'));
      foreach ($policy->getContentModels() as $pid => $info) {
        $cm_options[$pid] = array('title' => $info['name']);

        $cm_namespace = $info['namespace'];
        if (!empty($cm_namespace)) {
          $cm_namespace = $info['namespace'];
          $cm_namespace = explode(':', $info['namespace']);
          $namespace = trim($cm_namespace[0]);
          $ns_options[$namespace] = $namespace;
        }
      }
    }

    return array(
      'fs' => array(
        '#type' => 'fieldset',
        '#title' => t('DOI Batch Journal Importer'),
        '#description' => t('Provide either a list of Digital Object Identifiers in a file, or enter DOIs in the textarea below. In either case, the DOIs in the list should be separated by either whitespace or commas (or some combination thereof). NOTE: Presently, only journal (articles) are supported.'),
        'file' => array(
          '#type' => 'managed_file',
          '#title' => t('File of DOIs'),
          '#description' => t('A file listing Digital Object Identifiers (with ".txt" extension).'),
          '#upload_validators' => array(
            'file_validate_extensions' => array('txt'),
          ),
        ),
        'textarea' => array(
          '#type' => 'textarea',
          '#title' => t('DOIs'),
          '#description' => t('Provide a list Digital Object Identifiers (DOI) - eg. 10.1016/j.jorganchem.2011.11.018, 10.1016/j.tiv.2011.10.017, 10.1111/j.1540-4560.2012.01733.x'),
        ),
        'ids' => array(
          '#type' => 'value',
          '#value' => array(),
        ),
        'fs' => array(
          '#type' => 'fieldset',
          '#title' => t('Content Model'),
          '#description' => t('The content model(s) to assign to the imported objects.'),
          'content_model' => array(
            '#type' => 'tableselect',
            '#header' => array('title' => t('Name')),
            '#multiple' => TRUE,
            '#options' => $cm_options,
          ),
        ),
        'submit' => array(
          '#type' => 'submit',
          '#value' => t('Import'),
        ),
      ),
      '#attributes' => array(
        'enctype' => 'multipart/form-data',
      ),
    );
  }

  /**
   * Determine if we're ready to run our batch process.
   *
   * @see IslandoraBatchImporter::readyForBatch()
   */
  public static function readyForBatch(array &$form_state) {
    $text = trim($form_state['values']['textarea']);
    $file = file_load($form_state['values']['file']);
    if ($file) {
      $file_dois = trim(file_get_contents(drupal_realpath($file->uri)));
      if ($text) {
        $text .= ' ' . $file_dois;
      }
      else {
        $text = $file_dois;
      }
      file_delete($file);
      unset($form_state['values']['file']);
    }

    if ($text) {
      $form_state['values']['ids'] = array_unique(preg_split("/[\s,]+/", $text));
    }

    $content_models = array_filter($form_state['values']['content_model']);
    if (empty($content_models)) {
      form_set_error('content_model', t('Need to select at least one content model.'));
    }

    return !empty($form_state['values']['ids']) && !empty($content_models);
  }

  /**
   * Get the required info to pass into the file parameter of the constructor.
   *
   * @see IslandoraBatchImporter::getBatchInfo()
   */
  public static function getBatchInfo(array &$form_state) {
    // Grab the content models.
    // Cast to an array, in case there are none?
    $content_models = array_filter((array) $form_state['values']['content_model']);

    $ids = $form_state['values']['ids'];

    $info = array(
      'ids' => $ids,
      'content_models' => $content_models,
    );

    return $info;
  }
}

/**
 * DOI import object.
 *
 * Actually does the heavy-lifting during the import.
 * @author adam
 */
class DOIImportObject extends IslandoraImportObject {

  /**
   * Generated MODS Document.
   *
   * @var SimpleXMLElement
   */
  protected $mods;

  /**
   * An AbstractObject for use in calling hooks... Should *NOT* be ingested.
   *
   * @var AbstractObject
   */
  protected $skeleton;

  /**
   * Constructor.
   */
  protected function __construct($source) {
    parent::__construct($source);

    $this->contentModel = (array) $this->source->content_model;

    $this->skeleton = islandora_get_tuque_connection()->repository->constructObject('temp:pid');
    $this->skeleton->models = $this->contentModel;
  }

  /**
   * Get an item from the source.
   *
   * @see IslandoraImportObject::getOne()
   */
  public static function getOne(&$info) {
    $id = array_shift($info['ids']);

    module_load_include('inc', 'islandora_doi', 'includes/utilities');
    $response = islandora_doi_perform_request($id);
    $headers = array_change_key_case($response->headers);
    if ($response->data &&
      strpos($headers['content-type'], 'text/html') === FALSE &&
      strpos($response->data, "Malformed DOI") === FALSE) {

      $crossref_xml = simplexml_load_string($response->data);
      // Totally don't support books so let's not attempt to make records that
      // are actually books!
      if ($crossref_xml->doi_record->crossref->error || $crossref_xml->doi_record->crossref->book) {
        return FALSE;
      }
      else {
        $record = $response->data;
        $record->content_models = $info['content_models'];
        return new static($record);
      }
    }
    else {
      return FALSE;
    }
  }

  /**
   * Generates a MODS document repersenting the imported data.
   *
   * @see IslandoraImportObject::getMODS()
   */
  public function getMODS() {
    if ($this->mods === NULL) {
      // @todo Implement book support.
      module_load_include('inc', 'islandora_doi', 'includes/utilities');
      $doi_xml = simplexml_load_string($this->source);
      $this->mods = islandora_doi_transform_to_mods($doi_xml)->asXML();
    }

    return $this->mods;
  }
}
