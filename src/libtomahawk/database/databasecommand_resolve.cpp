/* === This file is part of Tomahawk Player - <http://tomahawk-player.org> ===
 *
 *   Copyright 2010-2011, Christian Muehlhaeuser <muesli@tomahawk-player.org>
 *
 *   Tomahawk is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   Tomahawk is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Tomahawk. If not, see <http://www.gnu.org/licenses/>.
 */

#include "databasecommand_resolve.h"

#include "artist.h"
#include "album.h"
#include "pipeline.h"
#include "sourcelist.h"
#include "utils/logger.h"

using namespace Tomahawk;


DatabaseCommand_Resolve::DatabaseCommand_Resolve( const query_ptr& query )
    : DatabaseCommand()
    , m_query( query )
{
    Q_ASSERT( Pipeline::instance()->isRunning() );
}


DatabaseCommand_Resolve::~DatabaseCommand_Resolve()
{
}


void
DatabaseCommand_Resolve::exec( DatabaseImpl* lib )
{
    /*
     *        Resolving is a 2 stage process.
     *        1) find list of trk/art/alb IDs that are reasonable matches to the metadata given
     *        2) find files in database by permitted sources and calculate score, ignoring
     *           results that are less than MINSCORE
     */

    if ( !m_query->resultHint().isEmpty() )
    {
        qDebug() << "Using result-hint to speed up resolving:" << m_query->resultHint();

        Tomahawk::result_ptr result = lib->resultFromHint( m_query );
        /*        qDebug() << "Result null:" << result.isNull();
         *        qDebug() << "Collection null:" << result->collection().isNull();
         *        qDebug() << "Source null:" << result->collection()->source().isNull();*/
        if ( !result.isNull() && !result->collection().isNull() && result->collection()->source()->isOnline() )
        {
            QList<Tomahawk::result_ptr> res;
            res << result;
            emit results( m_query->id(), res );
            return;
        }
    }

    if ( m_query->isFullTextQuery() )
        fullTextResolve( lib );
    else
        resolve( lib );
}


void
DatabaseCommand_Resolve::resolve( DatabaseImpl* lib )
{
    QList<Tomahawk::result_ptr> res;
    typedef QPair<int, float> scorepair_t;

    // STEP 1
    QList< QPair<int, float> > artists = lib->searchTable( "artist", m_query->artist() );
    QList< QPair<int, float> > tracks = lib->searchTable( "track", m_query->track() );
    QList< QPair<int, float> > albums = lib->searchTable( "album", m_query->album() );

    if ( artists.length() == 0 || tracks.length() == 0 )
    {
        qDebug() << "No candidates found in first pass, aborting resolve" << m_query->artist() << m_query->track();
        emit results( m_query->id(), res );
        return;
    }

    // STEP 2
    TomahawkSqlQuery files_query = lib->newquery();

    QStringList artsl, trksl;
    for ( int k = 0; k < artists.count(); k++ )
        artsl.append( QString::number( artists.at( k ).first ) );
    for ( int k = 0; k < tracks.count(); k++ )
        trksl.append( QString::number( tracks.at( k ).first ) );

    QString artsToken = QString( "file_join.artist IN (%1)" ).arg( artsl.join( "," ) );
    QString trksToken = QString( "file_join.track IN (%1)" ).arg( trksl.join( "," ) );

    QString sql = QString( "SELECT "
                            "url, mtime, size, md5, mimetype, duration, bitrate, "  //0
                            "file_join.artist, file_join.album, file_join.track, "  //7
                            "file_join.composer, file_join.discnumber, "            //10
                            "artist.name as artname, "                              //12
                            "album.name as albname, "                               //13
                            "track.name as trkname, "                               //14
                            "composer.name as cmpname, "                            //15
                            "file.source, "                                         //16
                            "file_join.albumpos, "                                  //17
                            "artist.id as artid, "                                  //18
                            "album.id as albid, "                                   //19
                            "composer.id as cmpid "                                 //20
                            "FROM file, file_join, artist, track "
                            "LEFT JOIN album ON album.id = file_join.album "
                            "LEFT JOIN artist AS composer ON composer.id = file_join.composer "
                            "WHERE "
                            "artist.id = file_join.artist AND "
                            "track.id = file_join.track AND "
                            "file.id = file_join.file AND "
                            "(%1 AND %2)" )
         .arg( artsToken )
         .arg( trksToken );

    files_query.prepare( sql );
    files_query.exec();

    while ( files_query.next() )
    {
        source_ptr s;
        QString url = files_query.value( 0 ).toString();

        if ( files_query.value( 16 ).toUInt() == 0 )
        {
            s = SourceList::instance()->getLocal();
        }
        else
        {
            s = SourceList::instance()->get( files_query.value( 16 ).toUInt() );
            if( s.isNull() )
            {
                qDebug() << "Could not find source" << files_query.value( 16 ).toUInt();
                continue;
            }

            url = QString( "servent://%1\t%2" ).arg( s->userName() ).arg( url );
        }

        Tomahawk::result_ptr result = Tomahawk::Result::get( url );
        Tomahawk::artist_ptr artist =
                Tomahawk::Artist::get( files_query.value( 18 ).toUInt(), files_query.value( 12 ).toString() );
        Tomahawk::album_ptr album =
                Tomahawk::Album::get( files_query.value( 19 ).toUInt(), files_query.value( 13 ).toString(), artist );
        Tomahawk::artist_ptr composer =
                Tomahawk::Artist::get( files_query.value( 20 ).toUInt(), files_query.value( 15 ).toString() );

        result->setModificationTime( files_query.value( 1 ).toUInt() );
        result->setSize( files_query.value( 2 ).toUInt() );
        result->setMimetype( files_query.value( 4 ).toString() );
        result->setDuration( files_query.value( 5 ).toUInt() );
        result->setBitrate( files_query.value( 6 ).toUInt() );
        result->setArtist( artist );
        result->setComposer( composer );
        result->setAlbum( album );
        result->setDiscNumber( files_query.value( 11 ).toUInt() );
        result->setTrack( files_query.value( 14 ).toString() );
        result->setRID( uuid() );
        result->setAlbumPos( files_query.value( 17 ).toUInt() );
        result->setTrackId( files_query.value( 9 ).toUInt() );

        TomahawkSqlQuery attrQuery = lib->newquery();
        QVariantMap attr;

        attrQuery.prepare( "SELECT k, v FROM track_attributes WHERE id = ?" );
        attrQuery.bindValue( 0, result->trackId() );
        attrQuery.exec();
        while ( attrQuery.next() )
        {
            attr[ attrQuery.value( 0 ).toString() ] = attrQuery.value( 1 ).toString();
        }

        result->setAttributes( attr );
        result->setCollection( s->collection() );
        res << result;
    }

    emit results( m_query->id(), res );
}


void
DatabaseCommand_Resolve::fullTextResolve( DatabaseImpl* lib )
{
    QList<Tomahawk::result_ptr> res;
    typedef QPair<int, float> scorepair_t;

    // STEP 1
    QList< QPair<int, float> > artistPairs = lib->searchTable( "artist", m_query->fullTextQuery(), 20 );
    QList< QPair<int, float> > trackPairs = lib->searchTable( "track", m_query->fullTextQuery(), 20 );
    QList< QPair<int, float> > albumPairs = lib->searchTable( "album", m_query->fullTextQuery(), 20 );

    foreach ( const scorepair_t& artistPair, artistPairs )
    {
        TomahawkSqlQuery query = lib->newquery();

        QString sql = QString( "SELECT name FROM artist WHERE id = %1" ).arg( artistPair.first );
        query.prepare( sql );
        query.exec();

        QList<Tomahawk::artist_ptr> artistList;
        while ( query.next() )
        {
            Tomahawk::artist_ptr artist = Tomahawk::Artist::get( artistPair.first, query.value( 0 ).toString() );
            artistList << artist;
        }

        emit artists( m_query->id(), artistList );
    }
    foreach ( const scorepair_t& albumPair, albumPairs )
    {
        TomahawkSqlQuery query = lib->newquery();

        QString sql = QString( "SELECT album.name, artist.id, artist.name FROM album, artist WHERE artist.id = album.artist AND album.id = %1" ).arg( albumPair.first );
        query.prepare( sql );
        query.exec();

        QList<Tomahawk::album_ptr> albumList;
        while ( query.next() )
        {
            Tomahawk::artist_ptr artist = Tomahawk::Artist::get( query.value( 1 ).toUInt(), query.value( 2 ).toString() );
            Tomahawk::album_ptr album = Tomahawk::Album::get( albumPair.first, query.value( 0 ).toString(), artist );
            albumList << album;
        }

        emit albums( m_query->id(), albumList );
    }

    if ( artistPairs.length() == 0 && trackPairs.length() == 0 && albumPairs.length() == 0 )
    {
        qDebug() << "No candidates found in first pass, aborting resolve" << m_query->artist() << m_query->track();
        emit results( m_query->id(), res );
        return;
    }

    // STEP 2
    TomahawkSqlQuery files_query = lib->newquery();

    QStringList artsl, trksl, albsl;
    for ( int k = 0; k < artistPairs.count(); k++ )
        artsl.append( QString::number( artistPairs.at( k ).first ) );
    for ( int k = 0; k < trackPairs.count(); k++ )
        trksl.append( QString::number( trackPairs.at( k ).first ) );
    for ( int k = 0; k < albumPairs.count(); k++ )
        albsl.append( QString::number( albumPairs.at( k ).first ) );

    QString artsToken = QString( "file_join.artist IN (%1)" ).arg( artsl.join( "," ) );
    QString trksToken = QString( "file_join.track IN (%1)" ).arg( trksl.join( "," ) );
    QString albsToken = QString( "file_join.album IN (%1)" ).arg( albsl.join( "," ) );

    QString sql = QString( "SELECT "
                            "url, mtime, size, md5, mimetype, duration, bitrate, "  //0
                            "file_join.artist, file_join.album, file_join.track, "  //7
                            "file_join.composer, file_join.discnumber, "            //10
                            "artist.name as artname, "                              //12
                            "album.name as albname, "                               //13
                            "track.name as trkname, "                               //14
                            "composer.name as cmpname, "                            //15
                            "file.source, "                                         //16
                            "file_join.albumpos, "                                  //17
                            "artist.id as artid, "                                  //18
                            "album.id as albid, "                                   //19
                            "composer.id as cmpid "                                 //20
                            "FROM file, file_join, artist, track "
                            "LEFT JOIN album ON album.id = file_join.album "
                            "LEFT JOIN artist AS composer ON composer.id = file_join.composer "
                            "WHERE "
                            "artist.id = file_join.artist AND "
                            "track.id = file_join.track AND "
                            "file.id = file_join.file AND "
                            "%1" )
                        .arg( trackPairs.length() > 0 ? trksToken : QString( "0" ) );

    files_query.prepare( sql );
    files_query.exec();

    while ( files_query.next() )
    {
        source_ptr s;
        QString url = files_query.value( 0 ).toString();

        if ( files_query.value( 16 ).toUInt() == 0 )
        {
            s = SourceList::instance()->getLocal();
        }
        else
        {
            s = SourceList::instance()->get( files_query.value( 16 ).toUInt() );
            if( s.isNull() )
            {
                qDebug() << "Could not find source" << files_query.value( 16 ).toUInt();
                continue;
            }

            url = QString( "servent://%1\t%2" ).arg( s->userName() ).arg( url );
        }

        Tomahawk::result_ptr result = Tomahawk::Result::get( url );
        Tomahawk::artist_ptr artist =
                Tomahawk::Artist::get( files_query.value( 18 ).toUInt(), files_query.value( 12 ).toString() );
        Tomahawk::album_ptr album =
                Tomahawk::Album::get( files_query.value( 19 ).toUInt(), files_query.value( 13 ).toString(), artist );
        Tomahawk::artist_ptr composer =
                Tomahawk::Artist::get( files_query.value( 20 ).toUInt(), files_query.value( 15 ).toString() );

        result->setModificationTime( files_query.value( 1 ).toUInt() );
        result->setSize( files_query.value( 2 ).toUInt() );
        result->setMimetype( files_query.value( 4 ).toString() );
        result->setDuration( files_query.value( 5 ).toUInt() );
        result->setBitrate( files_query.value( 6 ).toUInt() );
        result->setArtist( artist );
        result->setComposer( composer );
        result->setAlbum( album );
        result->setDiscNumber( files_query.value( 11 ).toUInt() );
        result->setTrack( files_query.value( 14 ).toString() );
        result->setRID( uuid() );
        result->setAlbumPos( files_query.value( 17 ).toUInt() );
        result->setTrackId( files_query.value( 9 ).toUInt() );

        for ( int k = 0; k < trackPairs.count(); k++ )
        {
            if ( trackPairs.at( k ).first == (int)result->trackId() )
            {
                result->setScore( trackPairs.at( k ).second );
                break;
            }
        }

        TomahawkSqlQuery attrQuery = lib->newquery();
        QVariantMap attr;

        attrQuery.prepare( "SELECT k, v FROM track_attributes WHERE id = ?" );
        attrQuery.bindValue( 0, result->trackId() );
        attrQuery.exec();
        while ( attrQuery.next() )
        {
            attr[ attrQuery.value( 0 ).toString() ] = attrQuery.value( 1 ).toString();
        }

        result->setAttributes( attr );

        result->setCollection( s->collection() );
        res << result;
    }

    emit results( m_query->id(), res );
}
