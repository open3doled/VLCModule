#!/usr/bin/env python3
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET


def get_ns(tag: str) -> str:
    if tag.startswith("{"):
        return tag[1:].split("}", 1)[0]
    return ""


def qname(ns: str, name: str) -> str:
    return f"{{{ns}}}{name}" if ns else name


def ensure_ui_property(widget, ns, name, value, kind="string"):
    for prop in widget.findall(qname(ns, "property")):
        if prop.get("name") == name:
            child = list(prop)
            if child:
                child[0].text = value
            else:
                leaf = ET.SubElement(prop, qname(ns, kind))
                leaf.text = value
            return
    prop = ET.SubElement(widget, qname(ns, "property"), {"name": name})
    leaf = ET.SubElement(prop, qname(ns, kind))
    leaf.text = value


def make_radio_item(ns: str, row: int, column: int, name: str, text: str, colspan: int = 1):
    item_attrs = {"row": str(row), "column": str(column)}
    if colspan > 1:
        item_attrs["colspan"] = str(colspan)
    item = ET.Element(qname(ns, "item"), item_attrs)
    widget = ET.SubElement(item, qname(ns, "widget"), {"class": "QRadioButton", "name": name})
    prop = ET.SubElement(widget, qname(ns, "property"), {"name": "text"})
    label = ET.SubElement(prop, qname(ns, "string"))
    label.text = text
    return item
def patch_open_disk_ui(tree_root: Path):
    ui_path = tree_root / "modules/gui/qt/ui/open_disk.ui"
    tree = ET.parse(ui_path)
    root = tree.getroot()
    ns = get_ns(root.tag)

    grid = root.find(f".//{qname(ns, 'layout')}[@name='gridLayout']")
    if grid is None:
      raise SystemExit("open_disk.ui: gridLayout not found")

    has_open3d_radio = (
        grid.find(f"./{qname(ns, 'item')}[@row='1'][@column='1']/{qname(ns, 'widget')}[@name='open3dBluRayIsoRadioButton']")
        is not None
    )

    if not has_open3d_radio:
        row_shift = {"1": "2", "2": "3", "3": "4"}
        for item in grid.findall(qname(ns, "item")):
            row = item.get("row")
            if row in row_shift:
                item.set("row", row_shift[row])
        grid.append(make_radio_item(ns, 1, 1, "open3dBluRayIsoRadioButton", "3D Blu-ray / ISO (Open3DOLED)", colspan=4))

    if grid.find(f"./{qname(ns, 'item')}[@row='1'][@column='5']/{qname(ns, 'widget')}[@name='dvdsimple']") is None:
        for item in list(grid.findall(qname(ns, "item"))):
            widget = item.find(qname(ns, "widget"))
            if widget is not None and widget.get("name") == "dvdsimple":
                grid.remove(item)
                item.set("row", "1")
                item.set("column", "5")
                item.set("colspan", "4")
                grid.append(item)
                break

    hbox = root.find(f".//{qname(ns, 'layout')}[@name='horizontalLayout_5']")
    if hbox is None:
        raise SystemExit("open_disk.ui: horizontalLayout_5 not found")

    has_browse_image = any(
        item.find(f"./{qname(ns, 'widget')}[@name='browseImageButton']") is not None
        for item in hbox.findall(qname(ns, "item"))
    )
    if not has_browse_image:
        item = ET.SubElement(hbox, qname(ns, "item"))
        widget = ET.SubElement(item, qname(ns, "widget"), {"class": "QPushButton", "name": "browseImageButton"})
        prop = ET.SubElement(widget, qname(ns, "property"), {"name": "sizePolicy"})
        sizepolicy = ET.SubElement(prop, qname(ns, "sizepolicy"), {"hsizetype": "Maximum", "vsizetype": "Fixed"})
        ET.SubElement(sizepolicy, qname(ns, "horstretch")).text = "0"
        ET.SubElement(sizepolicy, qname(ns, "verstretch")).text = "0"
        prop = ET.SubElement(widget, qname(ns, "property"), {"name": "text"})
        label = ET.SubElement(prop, qname(ns, "string"))
        label.text = "Browse ISO..."

    device_label = root.find(f".//{qname(ns, 'widget')}[@name='deviceLabel']")
    if device_label is None:
        raise SystemExit("open_disk.ui: deviceLabel not found")
    ensure_ui_property(device_label, ns, "text", "Disc device / root / ISO")

    tabstops = root.find(qname(ns, "tabstops"))
    if tabstops is None:
        raise SystemExit("open_disk.ui: tabstops not found")
    tab_texts = [tab.text for tab in tabstops.findall(qname(ns, "tabstop"))]
    wanted = [
        "dvdRadioButton",
        "bdRadioButton",
        "open3dBluRayIsoRadioButton",
        "audioCDRadioButton",
        "vcdRadioButton",
        "dvdsimple",
        "deviceCombo",
        "ejectButton",
        "browseDiscButton",
        "browseImageButton",
        "titleSpin",
        "chapterSpin",
        "audioSpin",
        "subtitlesSpin",
    ]
    if tab_texts != wanted:
        for child in list(tabstops):
            tabstops.remove(child)
        for name in wanted:
            tabstop = ET.SubElement(tabstops, qname(ns, "tabstop"))
            tabstop.text = name

    ET.indent(tree, space=" ", level=0)
    ui_path.write_text('<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(root, encoding="unicode"))


def patch_open_panels_hpp(tree_root: Path):
    path = tree_root / "modules/gui/qt/components/open_panels.hpp"
    text = path.read_text()

    text = text.replace(
        "        Cdda,\n        BRD\n",
        "        Cdda,\n        BRD,\n        Open3dBRD\n",
    )
    if "QString normalizeOpen3DBlurayPath( const QString& ) const;" not in text:
        text = text.replace(
            "private:\n    Ui::OpenDisk ui;\n",
            "private:\n    QString normalizeOpen3DBlurayPath( const QString& ) const;\n"
            "    bool isOpen3DBluRaySelection() const;\n"
            "    Ui::OpenDisk ui;\n",
        )
    if "void browseImage();" not in text:
        text = text.replace(
            "private slots:\n    void browseDevice();\n",
            "private slots:\n    void browseDevice();\n    void browseImage();\n",
        )
    path.write_text(text)


def patch_open_panels_cpp(tree_root: Path):
    path = tree_root / "modules/gui/qt/components/open_panels.cpp"
    text = path.read_text()

    if '#include <QFileInfo>\n' not in text:
        text = text.replace('#include <QFileDialog>\n', '#include <QFileDialog>\n#include <QFileInfo>\n')

    if 'BUTTONACT( ui.open3dBluRayIsoRadioButton, updateButtons() );' not in text:
        text = text.replace(
            "    BUTTONACT( ui.bdRadioButton,      updateButtons() );\n",
            "    BUTTONACT( ui.bdRadioButton,      updateButtons() );\n"
            "    BUTTONACT( ui.open3dBluRayIsoRadioButton, updateButtons() );\n",
        )
    if 'BUTTONACT( ui.browseImageButton, browseImage() );' not in text:
        text = text.replace(
            "    BUTTONACT( ui.browseDiscButton, browseDevice() );\n",
            "    BUTTONACT( ui.browseDiscButton, browseDevice() );\n"
            "    BUTTONACT( ui.browseImageButton, browseImage() );\n",
        )

    new_update_buttons = """void DiscOpenPanel::updateButtons()
{
    const bool is_open3d_bluray = isOpen3DBluRaySelection();

    if ( ui.dvdRadioButton->isChecked() )
    {
        if( m_discType != Dvd )
        {
            setDrive( psz_dvddiscpath );
            m_discType = Dvd;
        }
        ui.titleLabel->setText( qtr(\"Title\") );
        ui.chapterLabel->show();
        ui.chapterSpin->show();
        ui.diskOptionBox_2->show();
        ui.dvdsimple->setEnabled( true );
    }
    else if ( ui.bdRadioButton->isChecked() || is_open3d_bluray )
    {
        const DiscType bluray_type = is_open3d_bluray ? Open3dBRD : BRD;
        if( m_discType != bluray_type )
        {
            setDrive( psz_dvddiscpath );
            m_discType = bluray_type;
            ui.dvdsimple->setChecked( !var_InheritBool( p_intf, \"bluray-menu\" ) );
        }
        ui.titleLabel->setText( qtr(\"Title\") );
        ui.chapterLabel->hide();
        ui.chapterSpin->hide();
        ui.diskOptionBox_2->hide();
        ui.dvdsimple->setEnabled( true );
    }
    else if ( ui.vcdRadioButton->isChecked() )
    {
        if( m_discType != Vcd )
        {
            setDrive( psz_vcddiscpath );
            m_discType = Vcd;
        }
        ui.titleLabel->setText( qtr(\"Entry\") );
        ui.chapterLabel->hide();
        ui.chapterSpin->hide();
        ui.diskOptionBox_2->show();
        ui.dvdsimple->setEnabled( false );
    }
    else /* CDDA */
    {
        if( m_discType != Cdda )
        {
            setDrive( psz_cddadiscpath );
            m_discType = Cdda;
        }
        ui.titleLabel->setText( qtr(\"Track\") );
        ui.chapterLabel->hide();
        ui.chapterSpin->hide();
        ui.diskOptionBox_2->hide();
        ui.dvdsimple->setEnabled( false );
    }

    ui.browseImageButton->setVisible( is_open3d_bluray );
    ui.browseImageButton->setEnabled( is_open3d_bluray );
    updateMRL();
}
"""
    text, count = re.subn(
        r"void DiscOpenPanel::updateButtons\(\)\n\{.*?\n\}\n\n#undef setDrive\n",
        new_update_buttons + "\n#undef setDrive\n",
        text,
        count=1,
        flags=re.S,
    )
    if count != 1:
        raise SystemExit("open_panels.cpp: updateButtons block not found")

    new_update_mrl = """void DiscOpenPanel::updateMRL()
{
    QString discPath;
    QStringList fileList;

    discPath = ui.deviceCombo->currentText();

    int tmp = ui.deviceCombo->findText( discPath );
    if( tmp != -1 &&  ui.deviceCombo->itemData( tmp ) != QVariant::Invalid )
        discPath = ui.deviceCombo->itemData( tmp ).toString();

    if( isOpen3DBluRaySelection() )
        discPath = normalizeOpen3DBlurayPath( discPath );

    /* MRL scheme */
    const char *scheme;
    /* DVD */
    if( ui.dvdRadioButton->isChecked() ) {
        if( !ui.dvdsimple->isChecked() )
            scheme = \"dvd\";
        else
            scheme = \"dvdsimple\";
    } else if ( ui.bdRadioButton->isChecked() )
        scheme = \"bluray\";
    else if ( isOpen3DBluRaySelection() )
        scheme = \"open3dbluraymvc\";
    /* VCD */
    else if ( ui.vcdRadioButton->isChecked() )
        scheme = \"vcd\";
    /* CDDA */
    else
        scheme = \"cdda\";

    char *mrl = vlc_path2uri( qtu(discPath), scheme );
    if( unlikely(mrl == NULL) )
        return;

    /* Title/chapter encoded in MRL */
    QString anchor = \"\";
    if( ui.titleSpin->value() > 0 ) {
        if( ui.dvdRadioButton->isChecked() || ui.bdRadioButton->isChecked() || isOpen3DBluRaySelection() ) {
            anchor = QString(\"#%1\").arg( ui.titleSpin->value() );
            if ( ui.chapterSpin->value() > 0 )
                anchor += QString(\":%1\").arg( ui.chapterSpin->value() );
        }
        else if ( ui.vcdRadioButton->isChecked() )
            anchor = QString(\"#%1\").arg( ui.titleSpin->value() );
    }

    emit methodChanged( \"disc-caching\" );

    fileList << (qfu(mrl) + anchor);
    free(mrl);

    QString opts = \"\";

    /* Input item options */
    if ( ui.dvdRadioButton->isChecked() || ui.vcdRadioButton->isChecked() )
    {
        if ( ui.audioSpin->value() >= 0 )
            opts += \" :audio-track=\" +
                QString(\"%1\").arg( ui.audioSpin->value() );
        if ( ui.subtitlesSpin->value() >= 0 )
            opts += \" :sub-track=\" +
                QString(\"%1\").arg( ui.subtitlesSpin->value() );
    }
    else if( ui.audioCDRadioButton->isChecked() )
    {
        if( ui.titleSpin->value() > 0 )
            opts += QString(\" :cdda-track=%1\").arg( ui.titleSpin->value() );
    }
    else if ( ui.bdRadioButton->isChecked() || isOpen3DBluRaySelection() )
    {
        if ( ui.dvdsimple->isChecked() )
            opts += \" :no-bluray-menu\";
    }
    emit mrlUpdated( fileList, opts );
}
"""
    text, count = re.subn(
        r"void DiscOpenPanel::updateMRL\(\)\n\{.*?\n\}\n\nvoid DiscOpenPanel::browseDevice\(\)\n",
        new_update_mrl + "\nvoid DiscOpenPanel::browseDevice()\n",
        text,
        count=1,
        flags=re.S,
    )
    if count != 1:
        raise SystemExit("open_panels.cpp: updateMRL block not found")

    browse_device_anchor = """void DiscOpenPanel::browseDevice()
{
    const QStringList schemes = QStringList(QStringLiteral(\"file\"));
    QString dir = QFileDialog::getExistingDirectoryUrl( this,
            qtr( I_DEVICE_TOOLTIP ), p_intf->p_sys->filepath,
            QFileDialog::ShowDirsOnly, schemes ).toLocalFile();
    if( !dir.isEmpty() )
    {
        ui.deviceCombo->addItem( toNativeSepNoSlash( dir ) );
        ui.deviceCombo->setCurrentIndex( ui.deviceCombo->findText( toNativeSepNoSlash( dir ) ) );
    }

    updateMRL();
}
"""
    helper_block = """bool DiscOpenPanel::isOpen3DBluRaySelection() const
{
    return ui.open3dBluRayIsoRadioButton->isChecked();
}

QString DiscOpenPanel::normalizeOpen3DBlurayPath( const QString& inputPath ) const
{
    QFileInfo info( inputPath );
    if( !info.exists() )
        return inputPath;

    if( info.isFile() )
    {
        if( info.suffix().compare( QStringLiteral( \"iso\" ), Qt::CaseInsensitive ) == 0 )
            return info.canonicalFilePath();

        if( info.fileName().compare( QStringLiteral( \"index.bdmv\" ), Qt::CaseInsensitive ) == 0 )
        {
            const QDir bdmvDir = info.dir();
            if( bdmvDir.dirName().compare( QStringLiteral( \"BDMV\" ), Qt::CaseInsensitive ) == 0 )
                return QFileInfo( QDir( bdmvDir.absolutePath() ).filePath( QStringLiteral( \"..\" ) ) ).canonicalFilePath();
        }
        return info.canonicalFilePath();
    }

    if( info.isDir() )
    {
        QDir dir( info.canonicalFilePath() );
        if( dir.dirName().compare( QStringLiteral( \"BDMV\" ), Qt::CaseInsensitive ) == 0 &&
            QFileInfo( dir.filePath( QStringLiteral( \"index.bdmv\" ) ) ).exists() )
        {
            return QFileInfo( dir.filePath( QStringLiteral( \"..\" ) ) ).canonicalFilePath();
        }

        if( QFileInfo( dir.filePath( QStringLiteral( \"BDMV/index.bdmv\" ) ) ).exists() )
            return dir.absolutePath();
    }

    return info.canonicalFilePath();
}

"""
    if helper_block not in text:
        text = text.replace(browse_device_anchor, helper_block + browse_device_anchor)

    if "void DiscOpenPanel::browseImage()" not in text:
        browse_image_block = """void DiscOpenPanel::browseImage()
{
    const QStringList schemes = QStringList(QStringLiteral(\"file\"));
    const QUrl startUrl = p_intf->p_sys->filepath;
    const QUrl fileUrl = QFileDialog::getOpenFileUrl( this,
            qtr( \"Select a Blu-ray ISO image\" ), startUrl,
            qtr( \"Blu-ray ISO image (*.iso);;All files (*)\" ), nullptr,
            QFileDialog::DontUseNativeDialog, schemes );
    const QString path = fileUrl.toLocalFile();
    if( path.isEmpty() )
        return;

    p_intf->p_sys->filepath = fileUrl;
    ui.open3dBluRayIsoRadioButton->setChecked( true );
    const QString nativePath = toNativeSepNoSlash( path );
    ui.deviceCombo->addItem( nativePath );
    ui.deviceCombo->setCurrentIndex( ui.deviceCombo->findText( nativePath ) );
    updateMRL();
}

"""
        text = text.replace("void DiscOpenPanel::eject()\n", browse_image_block + "void DiscOpenPanel::eject()\n")

    text = text.replace(
        "    const QUrl startUrl = QUrl::fromLocalFile( p_intf->p_sys->filepath );\n",
        "    const QUrl startUrl = p_intf->p_sys->filepath;\n",
    )
    if "    p_intf->p_sys->filepath = fileUrl;\n    ui.open3dBluRayIsoRadioButton->setChecked( true );\n" not in text:
        text = text.replace(
            "    ui.open3dBluRayIsoRadioButton->setChecked( true );\n",
            "    p_intf->p_sys->filepath = fileUrl;\n    ui.open3dBluRayIsoRadioButton->setChecked( true );\n",
            1,
        )

    path.write_text(text)


def patch_tree(tree_root: Path):
    patch_open_disk_ui(tree_root)
    patch_open_panels_hpp(tree_root)
    patch_open_panels_cpp(tree_root)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("Usage: patch_vlc3_open3d_disc_ui.py /path/to/vlc-tree")
    patch_tree(Path(sys.argv[1]))


if __name__ == "__main__":
    main()
