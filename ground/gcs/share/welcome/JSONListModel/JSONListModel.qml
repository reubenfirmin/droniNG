/* JSONListModel - a QML ListModel with JSON and JSONPath support
 *
 * Copyright (c) 2012 Romain Pokrzywka (KDAB) (romain@kdab.com)
 * Licensed under the MIT licence (http://opensource.org/licenses/mit-license.php)
 */

import QtQuick 2.0
import "jsonpath.js" as JSONPath

Item {
    property string source: ""
    property string json: ""
    property string query: ""
    property string errorString: ""

    property ListModel model : ListModel { id: jsonModel }
    property alias count: jsonModel.count

    onSourceChanged: {
        errorString = "";
        json = "";
        if (source === "")
            return;
        var requestedSource = source;
        var xhr = new XMLHttpRequest;
        xhr.open("GET", source);
        xhr.onreadystatechange = function() {
            if (xhr.readyState == XMLHttpRequest.DONE && source === requestedSource) {
                if (xhr.status >= 200 && xhr.status < 300)
                    json = xhr.responseText;
                else
                    errorString = qsTr("Results unavailable");
            }
        }
        xhr.send();
    }

    onJsonChanged: updateJSONModel()
    onQueryChanged: updateJSONModel()

    function updateJSONModel() {
        jsonModel.clear();
        errorString = "";

        if ( json === "" )
            return;

        try {
            var objectArray = parseJSONString(json, query);
            for ( var key in objectArray ) {
                var jo = objectArray[key];
                jsonModel.append( jo );
            }
        } catch (error) {
            jsonModel.clear();
            errorString = qsTr("Results unavailable");
        }
    }

    function parseJSONString(jsonString, jsonPathQuery) {
        var objectArray = JSON.parse(jsonString);
        if ( jsonPathQuery !== "" )
            objectArray = JSONPath.jsonPath(objectArray, jsonPathQuery);

        return objectArray;
    }
}
