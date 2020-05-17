#include "svgparser.h"
#include <QDebug>
#include <QRegExp>


SVGParser::SVGParser(QObject *parent) : QObject(parent)
{
    _rootItem = nullptr;
}

CPrimitive * SVGParser::getRootItem()
{
    return _rootItem;
}

/**
* @brief Разбираем переданный файл
* @param file
* @return
*/
SVGParser::ParseStatus SVGParser::parse(QIODevice * device)
{
    if ( !device->open(QIODevice::ReadOnly|QIODevice::Text) ) {
        qWarning()<<"Device not opened!";
        return PS_NOFILE;
    }

    _globalMatrix = CMatrix(3,3);

    _cssParser = new CSS::CssParser(this); //-- Парсер стилей

    _rootItem = new CGroup();
    CPrimitive * currentLevel = _rootItem;

    _xml = new QXmlStreamReader(device);

    while ( !_xml->atEnd() ) {
        if ( _xml->hasError() ) {
            device->close();
            return PS_SYNTAXERROR;
        }
        QXmlStreamReader::TokenType token = _xml->readNext();
        if ( token==QXmlStreamReader::StartDocument ) continue;
        if ( token==QXmlStreamReader::StartElement ) {
            if ( _xml->name()=="svg" ) { //-- svg сам по себе как группа, парсим
                parseGroup(currentLevel, _xml->attributes().value("transform").toString());
            } else
            if ( _xml->name()=="g" ) {
                CGroup * g = new CGroup();
                currentLevel = static_cast<CPrimitive*>(CNodeInterface::levelDown(currentLevel, g));
                parseGroup(g, _xml->attributes().value("transform").toString());
            } else
            if ( _xml->name()=="path" ) {
                parsePath(currentLevel, _xml);
            } else
            if ( _xml->name()=="rect" ) {
                parseRect(currentLevel, _xml);
            } else
            if ( _xml->name()=="line" ) {
                parseLine(currentLevel, _xml);
            } else
            if ( _xml->name()=="image" ) {
                parseImage(currentLevel, _xml);
            } else
            if ( _xml->name()=="style" ) {
                _xml->readNext();
                parseCss(_xml->text().toString());
            }
        } else
        if ( token == QXmlStreamReader::EndElement ) {
            if ( _xml->name()=="svg" ) {

            } else
            if ( _xml->name()=="g" ) {
                currentLevel = static_cast<CPrimitive*>(CNodeInterface::levelUp(currentLevel));
            }
        }
    }

    //-- Переходим на самый-самый верх т.к. _rootItem будет в конце указывать на первый элемент
    if ( _rootItem->up!=nullptr ) _rootItem=static_cast<CPrimitive*>(_rootItem->up);

    return PS_OK;
}

/**
* @brief Парсим трансформации и отдаём в виде матрицы
* @return
*/
CMatrix SVGParser::parseTransform(QString transform)
{
    CMatrix  matrix(3, 3);

    if ( transform!="" ) {
        QRegExp rxTransform("(\\w*)\\((.*)\\)"); //-- получаем тип трансформации и её параметры
        QRegExp rxTransformParams("([-+]?\\d{1,10}(\\.\\d{1,10})?(e[+-]\\d{1,10})?)"); //-- Разбираем параметры на отдельные

        if (rxTransform.indexIn(transform, 0)==-1) throw 33;

        QString command = rxTransform.cap(1);

        CMatrix::TMatrix mParams;

        int pos =0, i =0;
        while (( pos = rxTransformParams.indexIn(rxTransform.cap(2), pos)) != -1) {
            mParams.insert( i, rxTransformParams.cap(1).toDouble() );
            i++;
            pos += rxTransformParams.matchedLength();
        }

        if (command == "matrix") { //-- готовая матрица
            if ( mParams.count() <6 ) throw 23;
            matrix.set(2, 3, mParams, CMatrix::SET_BY_COLS);
        } else
        if (command == "translate") {
            if ( mParams.count() <2 ) throw 24;
            matrix.translate(mParams[0], mParams[1]);
        } else {
            throw 25;
        }
    }

    return matrix;

}

bool SVGParser::parseGroup(CPrimitive * level, QString transforms)
{
    Q_UNUSED(level)
    _globalMatrix = parseTransform(transforms); //TODO: Домножать новую, что бы не затирать прежнюю

    return true;
}

bool SVGParser::parsePath(CPrimitive * level, QXmlStreamReader * xml)
{
    CPath * path = new CPath();
    CNodeInterface::addNext(level, path);

    QString pathD = xml->attributes().value("d").toString();
    QString transforms = xml->attributes().value("transform").toString();

    CMatrix matrix = parseTransform(transforms);

    CSS::Style style = parseStyle(xml);
    path->setStyles(style);

    CPoint openPathCoords(0,0); //-- Запоминаем в каких координатах открыли путь, что бы потом можно было закрыть

    QString allowedCommands = "mcslvhvazMCSLVHAZ";

    QRegExp rxCommands(QString("([%1])([^%1]*)").arg(allowedCommands)); //-- Ищем команды и берём их параметры
    QRegExp rxCommandParams("([-+]?\\d{1,10}(\\.\\d{1,10})?(e[+-]\\d{1,10})?)"); //-- Разбираем параметры на отдельные

    QList<CPoint> prevPoints; //-- Запоминаем список точек от предыдущей команды, т.к. они могут потребоваться для текущей
    QList<double> params; //-- Список параметров у текущей команды

    int posCommands = 0;
    int posCommandParams = 0;

    CPoint lastPoint(0,0); //-- Крайняя точка крайнего перемещения, нужна для всех относительных команд

    while (( posCommands = rxCommands.indexIn(pathD, posCommands)) != -1) {
        posCommands+= rxCommands.matchedLength();

        QString command = rxCommands.cap(1);
        //qInfo()<<command;

        //-- Парсим все параметры команды в один список       
        params.clear();
        posCommandParams =0;
        while (( posCommandParams = rxCommandParams.indexIn(rxCommands.cap(2), posCommandParams)) != -1) {
            params.append(rxCommandParams.cap(1).toDouble());            
            posCommandParams+= rxCommandParams.matchedLength();
        }

        //-- Ну и погнали парсить сами команды
        if ( (command=="M")||(command=="m") ) { //-- Перенос пера абсолютный/относительный
            if ( params.count()%2!=0 ) throw 45; //-- Должно быть пропорционально 2
            for (int pi=0;  pi<params.count(); pi+=2) {
                CPoint p1(params[pi], params[pi+1]);

                if ( command=="m" ) {
                    p1.add(lastPoint);
                }
                lastPoint = p1;
                p1.transform(matrix);

                if ( !openPathCoords.isZero() ) { //-- Будем считать, что следующие параметры после первых двух это линии
                    CLine * line = new CLine(_globalCoords, p1);
                    line->setStyles(style);
                    CNodeInterface::addNext(path, line);
                } else {
                    openPathCoords.set(lastPoint);
                }

                prevPoints.clear();
                prevPoints<<p1;

                _globalCoords = p1;
            }
        } else
        if ( (command=="C") || (command=="c") ) { //-- Кривая безье кубическая
            if (params.count()%6!=0) throw 45; //-- Должно быть пропорционально 6

            if (openPathCoords.isZero()) openPathCoords = lastPoint;
            for (int pi=0;  pi<params.count(); pi+=6) {
                CPoint p1(_globalCoords);
                CPoint p2(params[0+pi], params[1+pi]);
                CPoint p3(params[2+pi], params[3+pi]);
                CPoint p4(params[4+pi], params[5+pi]);
                if ( command=="c" ) {
                    p2.add(lastPoint);
                    p3.add(lastPoint);
                    p4.add(lastPoint);
                }
                lastPoint = p4;
                p2.transform(matrix);
                p3.transform(matrix);
                p4.transform(matrix);

                prevPoints.clear();
                prevPoints<<p1<<p2<<p3<<p4;

                CBezier * bezier = new CBezier(p1, p2, p3, p4);
                bezier->setStyles(style);
                CNodeInterface::addNext(path, bezier);
                _globalCoords = p4;
            }

        } else
        if ( (command=="S") || (command=="s") ) { //-- Кривая безье кубическая с началом у конца предыдущей
            if ( params.count()%4!=0 ) throw 45; //-- Должно быть пропорционально 4
            if (openPathCoords.isZero()) openPathCoords = lastPoint;

            for (int pi=0; pi<params.count(); pi+=4) {
                CPoint p1 = prevPoints[prevPoints.count()-1];
                CPoint p2 = (prevPoints.count()<=1)? _globalCoords : prevPoints[prevPoints.count()-2];
                CPoint p3(params[0+pi], params[1+pi]);
                CPoint p4(params[2+pi], params[3+pi]);
                p2.reflectP(p1); //-- Отражаем контрол относительно стартовой точки
                if (command=="s") {
                    p3.add(lastPoint);
                    p4.add(lastPoint);
                }
                lastPoint = p4;
                p3.transform(matrix);
                p4.transform(matrix);

                prevPoints.clear();
                prevPoints<<p1<<p2<<p3<<p4;

                CBezier * bezier = new CBezier(p1, p2, p3, p4);
                bezier->setStyles(style);
                CNodeInterface::addNext(path, bezier);
                _globalCoords = p4;
            }
        } else
        if ( (command=="L") || (command=="l") ) { //-- Линия
            if (params.count()%2!=0) throw 45; //-- Должно быть пропорционально 2

            for (int pi=0; pi<params.count(); pi+=2) {
                CPoint p(params[pi], params[pi+1]);

                if (command == "l") {
                    p.add(lastPoint);
                }
                lastPoint = p;
                if (openPathCoords.isZero()) openPathCoords.set(p);

                p.transform(matrix);

                prevPoints.clear();
                prevPoints<<p;

                CLine * line = new CLine(_globalCoords, p);
                line->setStyles(style);
                CNodeInterface::addNext(path, line);
                _globalCoords = p;
            }
        } else
        if ( (command=="H") || (command=="h") ) { //-- Горизонтальная линия
            if ( params.count()==0 ) throw 45; //-- Хотя бы один параметр да должен быть
            for (int pi=0; pi<params.count(); ++pi) {
                CPoint p(params[pi], lastPoint.y());
                if (command=="h") {
                    p.add(lastPoint.x(), 0);
                }
                lastPoint = p;
                if (openPathCoords.isZero()) openPathCoords.set(p);

                p.transform(matrix);

                prevPoints.clear();
                prevPoints<<p;

                CLine * line = new CLine(_globalCoords, p);
                line->setStyles(style);
                CNodeInterface::addNext(path, line);
                _globalCoords = p;
            }
        } else
        if ( (command=="V") || (command=="v") ) { //-- Вертикальная линия
            if ( params.count()==0 ) throw 45; //-- Хотя бы один параметр да должен быть
            for (int pi=0; pi<params.count(); ++pi) {
                CPoint p(lastPoint.x(), params[pi]);
                if ( command=="v" ) {
                     p.add(0, lastPoint.y());
                }
                lastPoint = p;
                if (openPathCoords.isZero()) openPathCoords.set(p);

                p.transform(matrix);

                prevPoints.clear();
                prevPoints<<p;

                CLine * line = new CLine(_globalCoords, p);
                line->setStyles(style);
                CNodeInterface::addNext(path, line);
                _globalCoords = p;
            }
        } else
        if ( (command=="A") || (command=="a") ) {  //TODO: Распарсить дуга круга/элипса!
            throw 55;

        } else
        if ( (command=="Z") || (command=="z") ) { //-- Закрываем путь

            path->setIsClosed(true);

            CPoint pe = openPathCoords;
            CPoint ps = lastPoint;

            ps.transform(matrix);
            pe.transform(matrix);

            if ( !ps.isEq(pe) ) { //-- Линию к началу, если отличается
                CLine * line = new CLine(ps, pe);
                line->setStyles(style);
                CNodeInterface::addNext(path, line);
            }

            lastPoint.set(openPathCoords);
            _globalCoords.set(pe);
            openPathCoords.set(0,0);

            //-- Всё, что дальше, считаем новым путём
            path = new CPath();
            path->setStyles(style);
            CNodeInterface::addNext(level, path);

        } else { //-- Какая-то другая комманда, о которой мы не знаем
            qWarning()<<"Unknow command"<<command;
            throw 35;
        }

    }

    if ( lastPoint.isEq(openPathCoords) ) path->setIsClosed(true);

    if ( path->first==nullptr ) { //-- Что бы не болтался лишний раз, если нет ничего
        CNodeInterface::removeFromLevel(path);
        delete path;
    }

    return true;
}

bool SVGParser::parseRect(CPrimitive *level, QXmlStreamReader *xml)
{
    CPath * rect = new CPath();
    CNodeInterface::addNext(level, rect);

    QString transforms = xml->attributes().value("transform").toString();
    CMatrix matrix = parseTransform(transforms);
    CSS::Style style = parseStyle(xml);

    double x = xml->attributes().value("x").toDouble();
    double y = xml->attributes().value("y").toDouble();
    double w = xml->attributes().value("width").toDouble();
    double h = xml->attributes().value("height").toDouble();

    CLine * t = new CLine(CPoint(x,y), CPoint(x+w, y));
    CLine * r = new CLine(CPoint(x+w,y), CPoint(x+w, y+h));
    CLine * b = new CLine(CPoint(x+w,y+h), CPoint(x, y+h));
    CLine * l = new CLine(CPoint(x,y+h), CPoint(x, y));


    t->setStyles(style);
    r->setStyles(style);
    b->setStyles(style);
    l->setStyles(style);

    CNodeInterface::addNext(rect, t);
    CNodeInterface::addNext(rect, r);
    CNodeInterface::addNext(rect, b);
    CNodeInterface::addNext(rect, l);

    return true;
}

/**
* @brief Собираем все в кучу стили для конкретного итема (локально заданные и глобальные)
* @param styles
* @param nameClass
* @param nameID
* @return
*/
CSS::Style SVGParser::parseStyle(QXmlStreamReader * xml)
{
    QString styles = xml->attributes().value("style").toString();
    QString nameClass = xml->attributes().value("class").toString();
    QString nameID = xml->attributes().value("id").toString();

    CSS::Block block(styles); //-- Распарсим локально заданные элементы стиля

    if (!block.parse()) {
        qWarning()<<"Problem parse css block";
    }

    //-- У элемента так же могут быть заданы конкретные стили как параметры
    QString stroke = xml->attributes().value("stroke").toString();
    QString fill = xml->attributes().value("fill").toString();

    if (stroke!="none" && stroke!="") block.set("stroke", stroke);
    if (fill!="none" && fill!="") block.set("fill", fill);

    CSS::Style style = _cssParser->applyStyles(QString(".%1").arg(nameClass), block); //-- Получим все стили для этого элемента, накладывая поверх локально заданные

    return style;
}


bool SVGParser::parseLine(CPrimitive * level, QXmlStreamReader * xml)
{    


    QString x1 = xml->attributes().value("x1").toString();
    QString y1 = xml->attributes().value("y1").toString();
    QString x2 = xml->attributes().value("x2").toString();
    QString y2 = xml->attributes().value("y2").toString();

    CPoint p1(x1.toDouble(), y1.toDouble());
    CPoint p2(x2.toDouble(), y2.toDouble());

    CLine * line = new CLine(p1, p2);
    line->setStyles(parseStyle(xml));

    CNodeInterface::addNext(level, line);

    return true;
}

/**
* @brief Парсим картинку
* @param level
* @param width
* @param height
* @param data
* @param transforms
* @return
*/
bool SVGParser::parseImage(CPrimitive *level, QXmlStreamReader * xml)
{
    QString width = xml->attributes().value("width").toString();
    QString height = xml->attributes().value("height").toString();
    QString data = xml->attributes().value("xlink:href").toString();
    QString transforms = xml->attributes().value("transform").toString();

    CMatrix transformsMatrix = parseTransform(transforms);

    transformsMatrix.multiplication(_globalMatrix);

    CPoint p1(0,0,0.0001);
    p1.transform(transformsMatrix); //-- Левый верхний угол картинки

    qInfo()<<"Parse image"<<width<<height;

    QRegExp rxType("data:(.+);(.+),");

    if (rxType.indexIn(data) == -1) {//-- Нипонятно что это есть
        qWarning()<<"Unknow image type!";
        return false;
    }

    data = data.remove(0, rxType.cap(0).length()); //--Убираем начало, т.к. это не часть данных
    data = data.remove(QRegExp("(\\n|\\t| )")); //-- Убираем всё лишнее

    CImage * image = new CImage(p1, rxType.cap(1), rxType.cap(2), data.toLocal8Bit(), transformsMatrix);


    CNodeInterface::addNext(level, image);

    return true;
}

bool SVGParser::parseCss(QString styles)
{
    return _cssParser->parse(styles);
}


