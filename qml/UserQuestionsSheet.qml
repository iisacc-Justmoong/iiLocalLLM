pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import LVRS 1.0 as LV

LV.Sheet {
    id: panel
    required property var inbox
    property var activeRequest: null
    property var drafts: []
    readonly property var questions: activeRequest ? activeRequest.request.input.questions : []
    readonly property string requestId: activeRequest ? activeRequest.request_id : ""
    objectName: "agentQuestionsSheet"
    title: qsTr("Agent questions")
    description: qsTr("Choose an option or write your own answer. You can leave questions unanswered.")
    visible: activeRequest !== null
    showCloseButton: false
    dismissOnBackground: false
    dismissOnEscape: false
    dismissOnDrag: false
    preferredWidth: 620
    preferredHeight: Math.min(720, Math.max(320, (parent ? parent.height : 0) - 64))

    function syncRequest() {
        const rows = inbox ? inbox.requests : []
        const next = rows.length > 0 ? rows[0] : null
        if ((next ? next.request_id : "") !== requestId) {
            drafts = []
            activeRequest = next
        }
    }
    function draft(index) {
        return drafts[index] || { selected: [], other: "", notes: "", preview: -1 }
    }
    function updateDraft(index, key, value) {
        const old = draft(index)
        if (key === "selected" ? JSON.stringify(old[key]) === JSON.stringify(value) : old[key] === value) return
        const next = { selected: old.selected, other: old.other, notes: old.notes, preview: old.preview }
        next[key] = value
        const all = drafts.slice()
        all[index] = next
        drafts = all
    }
    function selectOption(questionIndex, optionIndex) {
        let selected = draft(questionIndex).selected.slice()
        if (questions[questionIndex].multiSelect) {
            const existing = selected.indexOf(optionIndex)
            if (existing >= 0) selected.splice(existing, 1)
            else selected.push(optionIndex)
        } else {
            selected = [optionIndex]
            updateDraft(questionIndex, "other", "")
        }
        updateDraft(questionIndex, "selected", selected)
        updateDraft(questionIndex, "preview", optionIndex)
    }
    function sendAnswers() {
        // Question text is untrusted: keys such as __proto__ must remain data.
        const answers = Object.create(null)
        const annotations = Object.create(null)
        for (let i = 0; i < questions.length; ++i) {
            const question = questions[i]
            const entry = draft(i)
            let labels = question.options.filter(function(option, index) { return entry.selected.indexOf(index) >= 0 })
                .map(function(option) { return option.label })
            if (entry.other.trim().length > 0) {
                if (!question.multiSelect) labels = []
                labels.push(entry.other)
            }
            if (labels.length > 0) answers[question.question] = labels.join(", ")
            // These keys are fixed. Ordinary nested objects cross Qt's QVariantMap
            // boundary; a nested null-prototype object remains an opaque QJSValue.
            const annotation = ({})
            const chosen = entry.selected.length === 1 && entry.other.trim().length === 0
                ? question.options[entry.selected[0]] : null
            if (chosen && chosen.preview) annotation.preview = chosen.preview
            if (entry.notes.trim().length > 0) annotation.notes = entry.notes.trim()
            if (Object.keys(annotation).length > 0) annotations[question.question] = annotation
        }
        if (inbox) inbox.submit(requestId, answers, annotations)
    }
    Component.onCompleted: syncRequest()
    onInboxChanged: syncRequest()
    Connections {
        target: panel.inbox
        function onRequestsChanged() { panel.syncRequest() }
    }
    Shortcut {
        sequence: "Escape"
        enabled: panel.visible
        onActivated: if (panel.inbox) panel.inbox.reject(panel.requestId)
    }
    contentComponent: Component {
        ColumnLayout {
            width: panel.availableContentWidth
            spacing: 16
            LV.Label {
                Layout.fillWidth: true
                visible: panel.inbox && panel.inbox.requests.length > 1
                text: panel.inbox ? qsTr("%1 requests waiting").arg(panel.inbox.requests.length) : ""
                textFormat: Text.PlainText
            }
            Repeater {
                model: panel.questions
                ColumnLayout {
                    id: questionRow
                    required property var modelData
                    required property int index
                    readonly property var entry: panel.draft(index)
                    readonly property string preview: entry.preview >= 0 ? modelData.options[entry.preview].preview || "" : ""
                    Layout.fillWidth: true
                    spacing: 8
                    LV.Label {
                        Layout.fillWidth: true
                        text: questionRow.modelData.header
                        style: caption
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        sizeToContentHeight: true
                    }
                    LV.Label {
                        objectName: "agentQuestionText_" + questionRow.index
                        Layout.fillWidth: true
                        text: questionRow.modelData.question
                        style: body
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        sizeToContentHeight: true
                    }
                    Repeater {
                        model: questionRow.modelData.options
                        RowLayout {
                            id: optionRow
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignTop
                            readonly property bool selected: questionRow.entry.selected.indexOf(index) >= 0
                            LV.CheckBox {
                                objectName: "agentQuestionOption_" + questionRow.index + "_" + optionRow.index
                                visible: questionRow.modelData.multiSelect === true
                                checked: optionRow.selected
                                Layout.alignment: Qt.AlignTop
                                Accessible.name: optionRow.modelData.label
                                onClicked: panel.selectOption(questionRow.index, optionRow.index)
                                onActiveFocusChanged: if (activeFocus) panel.updateDraft(questionRow.index, "preview", optionRow.index)
                            }
                            LV.RadioButton {
                                objectName: "agentQuestionChoice_" + questionRow.index + "_" + optionRow.index
                                visible: questionRow.modelData.multiSelect !== true
                                checked: optionRow.selected
                                autoExclusive: true
                                Layout.alignment: Qt.AlignTop
                                Accessible.name: optionRow.modelData.label
                                onClicked: panel.selectOption(questionRow.index, optionRow.index)
                                onActiveFocusChanged: if (activeFocus) panel.updateDraft(questionRow.index, "preview", optionRow.index)
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                LV.Label {
                                    Layout.fillWidth: true
                                    text: optionRow.modelData.label
                                    style: body
                                    textFormat: Text.PlainText
                                    wrapMode: Text.Wrap
                                    sizeToContentHeight: true
                                }
                                LV.Label {
                                    Layout.fillWidth: true
                                    text: optionRow.modelData.description
                                    textFormat: Text.PlainText
                                    wrapMode: Text.Wrap
                                    sizeToContentHeight: true
                                }
                                TapHandler { onTapped: panel.selectOption(questionRow.index, optionRow.index) }
                                HoverHandler { onHoveredChanged: if (hovered) panel.updateDraft(questionRow.index, "preview", optionRow.index) }
                            }
                        }
                    }
                    LV.Label {
                        objectName: "agentQuestionPreview_" + questionRow.index
                        Layout.fillWidth: true
                        visible: questionRow.preview.length > 0
                        text: questionRow.preview
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        sizeToContentHeight: true
                    }
                    LV.TextEditor {
                        objectName: "agentQuestionOther_" + questionRow.index
                        property bool inputReady: false
                        Layout.fillWidth: true
                        filePath: ""
                        fieldMinHeight: 64
                        editorHeight: 64
                        placeholderText: qsTr("Your own answer")
                        text: questionRow.entry.other
                        Accessible.name: qsTr("Your answer to %1").arg(questionRow.modelData.question)
                        Component.onCompleted: {
                            editorItem.textFormat = TextEdit.PlainText
                            text = Qt.binding(function() { return panel.draft(questionRow.index).other })
                            inputReady = true
                        }
                        onTextEdited: function(value) {
                            if (!inputReady) return
                            panel.updateDraft(questionRow.index, "other", value)
                            if (!questionRow.modelData.multiSelect && value.trim().length > 0) panel.updateDraft(questionRow.index, "selected", [])
                        }
                    }
                    LV.TextEditor {
                        objectName: "agentQuestionNotes_" + questionRow.index
                        property bool inputReady: false
                        Layout.fillWidth: true
                        filePath: ""
                        fieldMinHeight: 64
                        editorHeight: 64
                        placeholderText: qsTr("Additional notes (optional)")
                        text: questionRow.entry.notes
                        Accessible.name: qsTr("Notes for %1").arg(questionRow.modelData.question)
                        Component.onCompleted: {
                            editorItem.textFormat = TextEdit.PlainText
                            text = Qt.binding(function() { return panel.draft(questionRow.index).notes })
                            inputReady = true
                        }
                        onTextEdited: function(value) {
                            if (inputReady) panel.updateDraft(questionRow.index, "notes", value)
                        }
                    }
                }
            }
            LV.Label {
                Layout.fillWidth: true
                visible: panel.inbox && panel.inbox.errorString.length > 0
                text: panel.inbox ? panel.inbox.errorString : ""
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                sizeToContentHeight: true
            }
            RowLayout {
                Layout.fillWidth: true
                LV.PushButton { objectName: "agentQuestionDecline"; tone: LV.AbstractButton.Borderless; text: qsTr("Decline"); onClicked: if (panel.inbox) panel.inbox.reject(panel.requestId) }
                LV.PushButton { objectName: "agentQuestionSkip"; tone: LV.AbstractButton.Default; text: qsTr("Skip"); onClicked: if (panel.inbox) panel.inbox.submit(panel.requestId, ({}), ({})) }
                Item { Layout.fillWidth: true }
                LV.PushButton { objectName: "agentQuestionSubmit"; text: qsTr("Send answers"); onClicked: panel.sendAnswers() }
            }
        }
    }
}
